#pragma once
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <unistd.h>

/** Available log levels in ascending severity order. **/
enum class LogLevel { DEBUG, INFO, WARN, ERR };

/**
 * Thread-safe, ANSI-coloured logger singleton.
 *
 * Use the convenience macros rather than calling log() directly:
 *   LOG_DEBUG(...)  LOG_INFO(...)  LOG_WARN(...)  LOG_ERR(...)
 *
 * Colours are automatically disabled when stderr is not a TTY
 * (e.g. when piped to a file).
 **/
class Logger {
	public:
		/** Return the global Logger instance. **/
		static Logger& get() {
			static Logger instance;
			return instance;
		}

		/** Set the minimum level that will be printed. Default: INFO. **/
		void setLevel(LogLevel lvl) { minLevel_ = lvl; }

		/** Return the current minimum level. **/
		LogLevel level() const { return minLevel_; }

		/**
		 * Emit one log line to stderr.
		 * @param lvl      Severity level.
		 * @param file     Source file (__FILE__).
		 * @param line     Source line (__LINE__).
		 * @param msg      Pre-formatted message string.
		 **/
		void log(LogLevel lvl, const char* file, int line, const std::string& msg) {
			if (lvl < minLevel_) return;

			auto now  = std::chrono::system_clock::now();
			auto secs = std::chrono::time_point_cast<std::chrono::seconds>(now);
			auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(now - secs).count();
			std::time_t tt = std::chrono::system_clock::to_time_t(secs);
			std::tm tm{};
			localtime_r(&tt, &tm);

			std::string_view sv(file);
			auto pos = sv.rfind("src/");
			std::string_view shortFile = (pos != std::string_view::npos) ? sv.substr(pos) : sv;

			bool color = isatty(fileno(stderr));

			static constexpr const char* RESET     = "\033[0m";
			static constexpr const char* DIM       = "\033[2m";
			static constexpr const char* CLR_DEBUG = "\033[1;34m";
			static constexpr const char* CLR_INFO  = "\033[1;32m";
			static constexpr const char* CLR_WARN  = "\033[1;33m";
			static constexpr const char* CLR_ERR   = "\033[1;31m";
			static constexpr const char* MSG_DEBUG = "\033[34m";
			static constexpr const char* MSG_INFO  = "\033[0m";
			static constexpr const char* MSG_WARN  = "\033[33m";
			static constexpr const char* MSG_ERR   = "\033[31m";

			const char* lvlClr = "", *msgClr = "", *dim = "", *reset = "";
			if (color) {
				reset = RESET; dim = DIM;
				switch (lvl) {
					case LogLevel::DEBUG: lvlClr = CLR_DEBUG; msgClr = MSG_DEBUG; break;
					case LogLevel::INFO:  lvlClr = CLR_INFO;  msgClr = MSG_INFO;  break;
					case LogLevel::WARN:  lvlClr = CLR_WARN;  msgClr = MSG_WARN;  break;
					case LogLevel::ERR:   lvlClr = CLR_ERR;   msgClr = MSG_ERR;   break;
				}
			}

			std::lock_guard<std::mutex> lock(mu_);
			std::fprintf(stderr,
					"%s[%04d-%02d-%02d %02d:%02d:%02d.%03lld]%s "
					"%s[%-5s]%s %s%s:%d%s %s%s%s\n",
					dim, tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
					tm.tm_hour, tm.tm_min, tm.tm_sec, (long long)ms, reset,
					lvlClr, levelStr(lvl), reset,
					dim, shortFile.data(), line, reset,
					msgClr, msg.c_str(), reset);
		}

	private:
		Logger() = default;
		LogLevel   minLevel_ = LogLevel::INFO;
		std::mutex mu_;

		static const char* levelStr(LogLevel l) {
			switch (l) {
				case LogLevel::DEBUG: return "DEBUG";
				case LogLevel::INFO:  return "INFO";
				case LogLevel::WARN:  return "WARN";
				case LogLevel::ERR:   return "ERROR";
			}
			return "?";
		}
};

/** Build a log message from a stream expression and emit it at the given level. **/
#define LOG_MSG(lvl, ...) \
	do { \
		std::ostringstream _oss; \
		_oss << __VA_ARGS__; \
		Logger::get().log(lvl, __FILE__, __LINE__, _oss.str()); \
	} while (0)

/** Emit a DEBUG-level message (only visible when --verbose is passed). **/
#define LOG_DEBUG(...) LOG_MSG(LogLevel::DEBUG, __VA_ARGS__)

/** Emit an INFO-level message (default visibility). **/
#define LOG_INFO(...)  LOG_MSG(LogLevel::INFO,  __VA_ARGS__)

/** Emit a WARN-level message. **/
#define LOG_WARN(...)  LOG_MSG(LogLevel::WARN,  __VA_ARGS__)

/** Emit an ERROR-level message. **/
#define LOG_ERR(...)   LOG_MSG(LogLevel::ERR,   __VA_ARGS__)
