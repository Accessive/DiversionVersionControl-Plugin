#include "subprocess.h"

#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace godot;

namespace diversion {

#ifdef _WIN32

static std::wstring to_wstring(const String &s) {
	Char16String utf16 = s.utf16();
	return std::wstring(reinterpret_cast<const wchar_t *>(utf16.get_data()));
}

// Quotes a single argument following the MSVCRT command-line parsing rules.
static std::wstring quote_arg(const std::wstring &arg) {
	if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
		return arg;
	}
	std::wstring result = L"\"";
	for (auto it = arg.begin();; ++it) {
		size_t num_backslashes = 0;
		while (it != arg.end() && *it == L'\\') {
			++it;
			++num_backslashes;
		}
		if (it == arg.end()) {
			result.append(num_backslashes * 2, L'\\');
			break;
		}
		if (*it == L'"') {
			result.append(num_backslashes * 2 + 1, L'\\');
		} else {
			result.append(num_backslashes, L'\\');
		}
		result.push_back(*it);
	}
	result.push_back(L'"');
	return result;
}

SubprocessResult subprocess_run(const String &exe, const PackedStringArray &args, const String &cwd) {
	SubprocessResult result;

	std::wstring cmdline = quote_arg(to_wstring(exe));
	for (int i = 0; i < args.size(); i++) {
		cmdline += L" " + quote_arg(to_wstring(args[i]));
	}

	SECURITY_ATTRIBUTES sa = {};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	HANDLE pipe_read = nullptr;
	HANDLE pipe_write = nullptr;
	if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) {
		return result;
	}
	SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = pipe_write;
	si.hStdError = pipe_write;
	si.hStdInput = INVALID_HANDLE_VALUE;

	PROCESS_INFORMATION pi = {};
	std::wstring wcwd = to_wstring(cwd);
	// CreateProcessW needs a mutable command-line buffer.
	std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
	cmdline_buf.push_back(L'\0');

	BOOL ok = CreateProcessW(
			nullptr, cmdline_buf.data(),
			nullptr, nullptr,
			TRUE, // inherit handles (the pipe)
			CREATE_NO_WINDOW,
			nullptr,
			wcwd.empty() ? nullptr : wcwd.c_str(),
			&si, &pi);

	CloseHandle(pipe_write);

	if (!ok) {
		CloseHandle(pipe_read);
		return result;
	}

	std::string raw;
	char buf[4096];
	DWORD bytes_read = 0;
	while (ReadFile(pipe_read, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
		raw.append(buf, bytes_read);
	}
	CloseHandle(pipe_read);

	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD exit_code = 0;
	GetExitCodeProcess(pi.hProcess, &exit_code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	result.exit_code = static_cast<int>(exit_code);
	result.output = String::utf8(raw.c_str(), raw.length());
	return result;
}

#else // POSIX

SubprocessResult subprocess_run(const String &exe, const PackedStringArray &args, const String &cwd) {
	SubprocessResult result;

	int pipefd[2];
	if (pipe(pipefd) != 0) {
		return result;
	}

	std::vector<std::string> arg_strings;
	arg_strings.push_back(exe.utf8().get_data());
	for (int i = 0; i < args.size(); i++) {
		arg_strings.push_back(args[i].utf8().get_data());
	}
	std::vector<char *> argv;
	for (std::string &s : arg_strings) {
		argv.push_back(s.data());
	}
	argv.push_back(nullptr);

	std::string cwd_utf8 = cwd.utf8().get_data();

	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return result;
	}
	if (pid == 0) {
		// Child.
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		if (!cwd_utf8.empty() && chdir(cwd_utf8.c_str()) != 0) {
			_exit(127);
		}
		execvp(argv[0], argv.data());
		_exit(127);
	}

	close(pipefd[1]);
	std::string raw;
	char buf[4096];
	ssize_t n;
	while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
		raw.append(buf, n);
	}
	close(pipefd[0]);

	int status = 0;
	waitpid(pid, &status, 0);
	if (WIFEXITED(status)) {
		result.exit_code = WEXITSTATUS(status);
	}
	result.output = String::utf8(raw.c_str(), raw.length());
	return result;
}

#endif

} // namespace diversion
