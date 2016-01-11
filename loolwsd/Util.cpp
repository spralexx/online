/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <sys/poll.h>
#ifdef __linux
#include <sys/prctl.h>
#endif

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <cassert>
#include <random>
#include <mutex>

#include <png.h>

#include <signal.h>

#include <Poco/Exception.h>
#include <Poco/Format.h>
#include <Poco/Net/WebSocket.h>
#include <Poco/Process.h>
#include <Poco/Timestamp.h>
#include <Poco/Thread.h>
#include <Poco/Util/Application.h>
#include <Poco/Environment.h>
#include <Poco/ConsoleChannel.h>

#include <Common.hpp>
#include "Util.hpp"
#include "Png.hpp"

// Callback functions for libpng

extern "C"
{
    static void user_write_status_fn(png_structp, png_uint_32, int)
    {
    }

    static void user_write_fn(png_structp png_ptr, png_bytep data, png_size_t length)
    {
        std::vector<char> *outputp = (std::vector<char> *) png_get_io_ptr(png_ptr);
        const size_t oldsize = outputp->size();
        outputp->resize(oldsize + length);
        memcpy(outputp->data() + oldsize, data, length);
    }

    static void user_flush_fn(png_structp)
    {
    }
}

volatile LOOLState TerminationState = LOOLState::LOOL_RUNNING;
volatile bool TerminationFlag = false;

namespace Util
{
namespace rng
{
    static std::random_device _rd;
    static std::mutex _rngMutex;
    static std::mt19937_64 _rng = std::mt19937_64(_rd());
    unsigned getNext()
    {
        std::unique_lock<std::mutex> lock(_rngMutex);
        return _rng();
    }
}
}

namespace Log
{
    static const Poco::Int64 epochStart = Poco::Timestamp().epochMicroseconds();
    static std::string SourceName;
    static std::string SourceId;

    std::string logPrefix()
    {
        Poco::Int64 usec = Poco::Timestamp().epochMicroseconds() - epochStart;

        const Poco::Int64 one_s = 1000000;
        const Poco::Int64 hours = usec / (one_s*60*60);
        usec %= (one_s*60*60);
        const Poco::Int64 minutes = usec / (one_s*60);
        usec %= (one_s*60);
        const Poco::Int64 seconds = usec / (one_s);
        usec %= (one_s);

        std::ostringstream stream;
        stream << Log::SourceId << '-' << std::setw(2) << std::setfill('0')
               << (Poco::Thread::current() ? Poco::Thread::current()->id() : 0) << ','
               << std::setw(2) << hours << ':' << std::setw(2) << minutes << ':'
               << std::setw(2) << seconds << "." << std::setw(6) << usec
               << ", ";

#ifdef __linux
        char buf[32]; // we really need only 16
        if (prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(buf), 0, 0, 0) == 0)
            stream << '[' << buf << "] ";
#endif

        return stream.str();
    }


    void initialize(const std::string& name)
    {
        SourceName = name;
        std::ostringstream oss;
        oss << SourceName << '-'
            << std::setw(5) << std::setfill('0') << Poco::Process::id();
        SourceId = oss.str();

        auto& logger = Poco::Logger::create(SourceName, new Poco::ColorConsoleChannel(), Poco::Message::PRIO_INFORMATION);

        // Configure the logger.
        // TODO: This should come from a file.
        try
        {
            // See Poco::Logger::setLevel docs for values.
            // Try: error, information, debug
            const auto level = Poco::Environment::get("LOOL_LOGLEVEL");
            logger.setLevel(level);
        }
        catch (Poco::NotFoundException& aError)
        {
        }

        info("Initializing " + name);
        info("Log level is [" + std::to_string(logger.getLevel()) + "].");
    }

    Poco::Logger& logger()
    {
        return Poco::Logger::get(SourceName);
    }

    void trace(const std::string& msg)
    {
        logger().trace(logPrefix() + msg);
    }

    void debug(const std::string& msg)
    {
        logger().debug(logPrefix() + msg);
    }

    void info(const std::string& msg)
    {
        logger().information(logPrefix() + msg);
    }

    void warn(const std::string& msg)
    {
        logger().warning(logPrefix() + msg);
    }

    void error(const std::string& msg)
    {
        logger().error(logPrefix() + msg + " (" + strerror(errno) + ").");
    }
}

namespace Util
{
    std::string encodeId(const unsigned number, const int padding)
    {
        std::ostringstream oss;
        oss << std::hex << std::setw(padding) << std::setfill('0') << number;
        return oss.str();
    }

    unsigned decodeId(const std::string& str)
    {
        unsigned id = 0;
        std::stringstream ss;
        ss << std::hex << str;
        ss >> id;
        return id;
    }

    std::string createRandomDir(const std::string& path)
    {
        Poco::File(path).createDirectories();
        for (;;)
        {
            const auto name = Util::encodeId(rng::getNext());
            Poco::File dir(Poco::Path(path, name));
            if (dir.createDirectory())
            {
                return name;
            }
        }
    }

    std::string createRandomFile(const std::string& path)
    {
        Poco::File(path).createDirectories();
        for (;;)
        {
            const auto name = Util::encodeId(rng::getNext());
            Poco::File file(Poco::Path(path, name));
            if (file.createFile())
            {
                return name;
            }
        }
    }

    bool windowingAvailable()
    {
#ifdef __linux
        return std::getenv("DISPLAY") != nullptr;
#endif

        return false;
    }

    bool encodePNGAndAppendToBuffer(unsigned char *pixmap, int width, int height, std::vector<char>& output, LibreOfficeKitTileMode mode)
    {
        png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);

        png_infop info_ptr = png_create_info_struct(png_ptr);

        if (setjmp(png_jmpbuf(png_ptr)))
        {
            png_destroy_write_struct(&png_ptr, nullptr);
            return false;
        }

        png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

        png_set_write_fn(png_ptr, &output, user_write_fn, user_flush_fn);
        png_set_write_status_fn(png_ptr, user_write_status_fn);

        png_write_info(png_ptr, info_ptr);

        switch (mode)
        {
        case LOK_TILEMODE_RGBA:
            break;
        case LOK_TILEMODE_BGRA:
            png_set_write_user_transform_fn (png_ptr, unpremultiply_data);
            break;
        default:
            assert(false);
        }

        for (int y = 0; y < height; ++y)
            png_write_row(png_ptr, pixmap + y * width * 4);

        png_write_end(png_ptr, info_ptr);

        png_destroy_write_struct(&png_ptr, &info_ptr);

        return true;
    }

    void shutdownWebSocket(Poco::Net::WebSocket& ws)
    {
        try
        {
            ws.shutdown();
        }
        catch (const Poco::IOException& exc)
        {
            Log::error("IOException: " + exc.message());
        }
    }

    std::string signalName(int signo)
    {
        switch (signo)
        {
#define CASE(x) case SIG##x: return #x
            CASE(HUP);
            CASE(INT);
            CASE(QUIT);
            CASE(ILL);
            CASE(ABRT);
            CASE(FPE);
            CASE(KILL);
            CASE(SEGV);
            CASE(PIPE);
            CASE(ALRM);
            CASE(TERM);
            CASE(USR1);
            CASE(USR2);
            CASE(CHLD);
            CASE(CONT);
            CASE(STOP);
            CASE(TSTP);
            CASE(TTIN);
            CASE(TTOU);
            CASE(BUS);
#ifdef SIGPOLL
            CASE(POLL);
#endif
            CASE(PROF);
            CASE(SYS);
            CASE(TRAP);
            CASE(URG);
            CASE(VTALRM);
            CASE(XCPU);
            CASE(XFSZ);
#ifdef SIGEMT
            CASE(EMT);
#endif
#ifdef SIGSTKFLT
            CASE(STKFLT);
#endif
#if defined(SIGIO) && SIGIO != SIGPOLL
            CASE(IO);
#endif
#ifdef SIGPWR
            CASE(PWR);
#endif
#ifdef SIGLOST
            CASE(LOST);
#endif
            CASE(WINCH);
#if defined(SIGINFO) && SIGINFO != SIGPWR
            CASE(INFO);
#endif
#undef CASE
        default:
            return std::to_string(signo);
        }
    }

    ssize_t writeFIFO(int nPipe, const char* pBuffer, ssize_t nSize)
    {
        ssize_t nBytes = -1;
        ssize_t nCount = 0;

        while(true)
        {
            nBytes = write(nPipe, pBuffer + nCount, nSize - nCount);
            if (nBytes < 0)
            {
                if (errno == EINTR || errno == EAGAIN)
                    continue;

                nCount = -1;
                break;
            }
            else if ( nCount + nBytes < nSize )
            {
                nCount += nBytes;
            }
            else
            {
                nCount = nBytes;
                break;
            }
        }

        return nCount;
    }

    ssize_t readFIFO(int nPipe, char* pBuffer, ssize_t nSize)
    {
        ssize_t nBytes;
        do
        {
            nBytes = read(nPipe, pBuffer, nSize);
        }
        while ( nBytes < 0 && errno == EINTR );

        return nBytes;
    }

    ssize_t readMessage(int nPipe, char* pBuffer, ssize_t nSize)
    {
        ssize_t nBytes = -1;
        struct pollfd aPoll;

        aPoll.fd = nPipe;
        aPoll.events = POLLIN;
        aPoll.revents = 0;

        int nPoll = poll(&aPoll, 1, 3000);
        if ( nPoll < 0 )
            goto ErrorPoll;

        if ( nPoll == 0 )
            errno = ETIME;

        if( (aPoll.revents & POLLIN) != 0 )
            nBytes = readFIFO(nPipe, pBuffer, nSize);

    ErrorPoll:
        return nBytes;
    }

    static
    void handleSignal(int aSignal)
    {
        Log::info() << "Signal received: " << strsignal(aSignal) << Log::end;
        TerminationFlag = true;
        TerminationState = ( aSignal == SIGTERM ? LOOLState::LOOL_ABNORMAL : LOOLState::LOOL_STOPPING );

        if (aSignal == SIGSEGV || aSignal == SIGBUS)
        {
            Log::error() << "\nSegfault! Stalling for 10 seconds to attach debugger. Use:\n"
                         << "sudo gdb --pid=" << Poco::Process::id() << "\n or \n"
                         << "sudo gdb --q --n --ex 'thread apply all backtrace full' --batch --pid="
                         << Poco::Process::id() << "\n" << Log::end;
            sleep(10);
        }
    }

    void setSignals(bool isIgnored)
    {
#ifdef __linux
        struct sigaction aSigAction;

        sigemptyset(&aSigAction.sa_mask);
        aSigAction.sa_flags = 0;
        aSigAction.sa_handler = (isIgnored ? SIG_IGN : handleSignal);

        sigaction(SIGTERM, &aSigAction, nullptr);
        sigaction(SIGINT, &aSigAction, nullptr);
        sigaction(SIGQUIT, &aSigAction, nullptr);
        sigaction(SIGHUP, &aSigAction, nullptr);

        if (getenv("LOOL_DEBUG"))
        {
            sigaction(SIGBUS, &aSigAction, nullptr);
            sigaction(SIGSEGV, &aSigAction, nullptr);
        }
#endif
    }
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
