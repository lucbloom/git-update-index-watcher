#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#pragma comment(lib, "Shell32.lib")

#include <resource.h>

NOTIFYICONDATA nid = {};
bool running = true;
bool paused = false;
std::vector<std::wstring> repos;
std::wstring iniPath;
int intervalSeconds = 10;

static std::wstring Utf8ToWstring(const std::string& s)
{
	int size_needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
	std::wstring wstr(size_needed, 0);
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &wstr[0], size_needed);
	return wstr;
}

static std::wstring RunCmd(const std::wstring& cmd, const std::wstring& cwd)
{
	HANDLE hStdOutRead = NULL, hStdOutWrite = NULL;
	SECURITY_ATTRIBUTES sa{ sizeof(sa), NULL, TRUE };
	if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0)) return L"";
	SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.hStdOutput = hStdOutWrite;
	si.hStdError = hStdOutWrite;
	si.wShowWindow = SW_HIDE;

	PROCESS_INFORMATION pi{};
	std::wstring cmdline = L"cmd.exe /C " + cmd;

	if (!CreateProcessW(NULL, cmdline.data(), NULL, NULL, TRUE,
		CREATE_NO_WINDOW, NULL,
		cwd.empty() ? NULL : cwd.c_str(),
		&si, &pi))
	{
		CloseHandle(hStdOutWrite);
		CloseHandle(hStdOutRead);
		return L"";
	}

	CloseHandle(hStdOutWrite);

	std::string output;
	char buffer[4096];
	DWORD bytesRead;
	while (ReadFile(hStdOutRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
	{
		buffer[bytesRead] = 0;
		output += buffer;
	}

	CloseHandle(hStdOutRead);
	WaitForSingleObject(pi.hProcess, INFINITE);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	return Utf8ToWstring(output);
}

static void LoadRepos()
{
	repos.clear();

	wchar_t buf[8192];
	DWORD len = GetPrivateProfileSectionW(L"Repos", buf, 8192, iniPath.c_str());

	if (len == 0) {
		WritePrivateProfileStringW(L"Settings", L"interval", L"10", iniPath.c_str());
		WritePrivateProfileStringW(L"Repos", L"island", L"C:\\dev\\island", iniPath.c_str());
		WritePrivateProfileStringW(L"Repos", L"island_config", L"C:\\dev\\island_config", iniPath.c_str());
		len = GetPrivateProfileSectionW(L"Repos", buf, 8192, iniPath.c_str());
	}

	// Load repos
	wchar_t* p = buf;
	while (*p) {
		std::wstring entry = p;
		size_t pos = entry.find(L'=');
		if (pos != std::wstring::npos) {
			std::wstring path = entry.substr(pos + 1);
			if (!path.empty()) {
				repos.push_back(path);
			}
		}
		p += entry.size() + 1;
	}

	// Load interval
	wchar_t intervalBuf[32] = {};
	if (GetPrivateProfileStringW(L"Settings", L"interval", L"10", intervalBuf, _countof(intervalBuf), iniPath.c_str()) > 0) {
		int val = _wtoi(intervalBuf);
		if (val > 0) {
			intervalSeconds = val;
		}
	}
}

static void RunChecksNow()
{
	for (auto& repoPath : repos) {
		std::wstring output = RunCmd(L"git diff --name-only", repoPath);
		std::wistringstream iss(output);
		std::wstring line;
		std::vector<std::wstring> allFiles;

		while (std::getline(iss, line)) {
			if (!line.empty()) { // crude filter for actual files
				allFiles.push_back(line);
			}
		}

		// Only run retouch logic if number of files is below threshold
		if (allFiles.size() > 20) {
			return;
		}

		std::vector<std::wstring> toRetouch;
		for (auto& line : allFiles) {
			if (line.empty()) {
				continue;
			}
			size_t firstQuote = line.find(L'\'');
			if (firstQuote == std::wstring::npos) {
				continue;
			}
			size_t secondQuote = line.find(L'\'', firstQuote + 1);
			if (secondQuote == std::wstring::npos) {
				continue;
			}
			if (line.find(L"the next time Git touches it") == std::wstring::npos) {
				continue;
			}
			auto path = line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
			DWORD attrs = GetFileAttributesW((repoPath + L'/' + path).c_str());
			if (attrs == INVALID_FILE_ATTRIBUTES) {
				continue;
			}
			if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
				continue;
			}
			toRetouch.push_back(path);
		}

		toRetouch.erase(
			std::remove_if(
				toRetouch.begin(), toRetouch.end(),
				[&](const std::wstring& f) {
					return std::find(allFiles.begin(), allFiles.end(), f) != allFiles.end();
				}),
			toRetouch.end()
		);

		// Restore files to fix Git false positive
		for (auto& f : toRetouch) {
			RunCmd(L"git checkout -- \"" + f + L"\"", repoPath);
		}
	}
}

static void WatchRepos()
{
	while (running) {
		if (!paused) {
			RunChecksNow();
		}
		std::this_thread::sleep_for(std::chrono::seconds(intervalSeconds));
	}
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_DESTROY:
		running = false;
		Shell_NotifyIcon(NIM_DELETE, &nid);
		PostQuitMessage(0);
		break;
	case WM_USER + 1:
		if (lParam == WM_RBUTTONUP) {
			HMENU menu = CreatePopupMenu();
			AppendMenu(menu, MF_STRING, 1, L"Reload Config");
			AppendMenu(menu, MF_STRING, 2, L"Open Config");
			AppendMenu(menu, MF_STRING, 3, paused ? L"Unpause" : L"Pause");
			AppendMenu(menu, MF_STRING, 4, L"Exit");
			POINT pt;
			GetCursorPos(&pt);
			SetForegroundWindow(hwnd);
			int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
			if (cmd == 1) {
				LoadRepos();
			}
			else if (cmd == 2) {
				ShellExecuteW(NULL, L"open", iniPath.c_str(), NULL, NULL, SW_SHOW);
			}
			else if (cmd == 3) {
				paused = !paused;
				wcscpy_s(nid.szTip, paused ? L"Git Watcher (paused)" : L"Git Watcher");
				Shell_NotifyIcon(NIM_MODIFY, &nid);
			}
			else if (cmd == 4) {
				DestroyWindow(hwnd);
			}

			DestroyMenu(menu);
		}
		else if (lParam == WM_LBUTTONUP) {
			// Left-click → run check immediately
			RunChecksNow();
		}
		break;
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
	const wchar_t CLASS_NAME[] = L"GitWatcher";
	WNDCLASS wc = { };
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = CLASS_NAME;
	RegisterClass(&wc);

	HWND hwnd = CreateWindowEx(0, CLASS_NAME, L"GitWatcher", 0,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		NULL, NULL, hInstance, NULL);

	nid.cbSize = sizeof(nid);
	nid.hWnd = hwnd;
	nid.uID = 1;
	nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;// | NIF_GUID;
	//CoCreateGuid(&nid.guidItem);
	nid.uCallbackMessage = WM_USER + 1;
	nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_TRAY));
	wcscpy_s(nid.szTip, L"Git Watcher");
	Shell_NotifyIcon(NIM_ADD, &nid);

	wchar_t appData[MAX_PATH];
	SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData);
	iniPath = std::wstring(appData) + L"\\GitWatcher";
	CreateDirectoryW(iniPath.c_str(), NULL);
	iniPath += L"\\repos.ini";
	LoadRepos();

	std::thread worker(WatchRepos);
	worker.detach();

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return 0;
}
