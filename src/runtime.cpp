/*
Trackspin
Copyright (C) 2026 Trackspin

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

// Плагин не содержит логики: он запускает агент (отдельную программу рядом с DLL), следит за ним
// и пересылает ему настройки источника. Агент завершается сам, когда закрывается OBS.

#include "runtime.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <httplib.h>
#include <obs-frontend-api.h>
#endif

namespace trackspin {

#ifdef _WIN32
namespace {

std::mutex g_mu;
PROCESS_INFORMATION g_proc = {};
std::string g_token;
std::string g_base_url;
std::string g_port_file;
std::string g_log_file;
RuntimeOptions g_opts;
std::thread g_watchdog;
std::atomic<bool> g_stopping{false};

// Microsoft Store build. The agent lives in the MSIX package; the package's agent copies this plugin into
// %ProgramData%\obs-studio\plugins\trackspin and leaves store-package.txt next to it (see agent/store_setup.cpp).
struct StoreInfo {
	bool store = false;
	std::string family; // package family name
	std::string sid;    // Windows user who installed the package
};
StoreInfo g_store;
bool g_inactive = false;         // Store package is gone: no agent, no source
bool g_remove_on_unload = false; // delete the plugin folder once OBS exits
std::string g_update_to;          // version the agent put into the OBS folder (Store update)
bool g_update_shown = false;

std::string random_token()
{
	std::random_device rd; // на Windows — криптостойкий источник (rand_s)
	char buf[33];
	for (int i = 0; i < 4; ++i)
		std::snprintf(buf + i * 8, 9, "%08x", rd());
	return buf;
}

std::wstring widen(const std::string &s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(n > 0 ? n - 1 : 0, L'\0');
	if (n > 0)
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
	return w;
}

std::string narrow(const std::wstring &w)
{
	if (w.empty())
		return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

// Folder of trackspin.dll with a trailing slash (UTF-8).
std::string module_dir()
{
	std::string dll = obs_get_module_binary_path(obs_current_module());
	size_t slash = dll.find_last_of("/\\");
	return dll.substr(0, slash + 1);
}

StoreInfo read_store_marker()
{
	StoreInfo info;
	std::ifstream f(widen(module_dir() + "store-package.txt"));
	std::string version;
	if (f && std::getline(f, info.family) && !info.family.empty()) {
		std::getline(f, version);
		std::getline(f, info.sid);
		info.store = true;
	}
	return info;
}

// Whether the package is installed for the current Windows user.
bool package_installed(const std::string &family)
{
	using Fn = LONG(WINAPI *)(PCWSTR, UINT32 *, PWSTR *, UINT32 *, WCHAR *);
	auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetPackagesByPackageFamily"));
	if (!fn)
		return true; // cannot tell; never remove the plugin on a guess
	UINT32 count = 0, len = 0;
	LONG rc = fn(widen(family).c_str(), &count, nullptr, &len, nullptr);
	if (rc != ERROR_SUCCESS && rc != ERROR_INSUFFICIENT_BUFFER)
		return true;
	return count > 0;
}

std::string current_user_sid()
{
	HANDLE tok = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
		return std::string();
	DWORD len = 0;
	GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
	std::string out;
	std::string buf(len, '\0');
	if (len && GetTokenInformation(tok, TokenUser, buf.data(), len, &len)) {
		LPWSTR s = nullptr;
		if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buf.data())->User.Sid, &s)) {
			out = narrow(s);
			LocalFree(s);
		}
	}
	CloseHandle(tok);
	return out;
}

// The package's execution alias: %LOCALAPPDATA%\Microsoft\WindowsApps\trackspin-agent.exe.
std::string store_alias_path()
{
	PWSTR la = nullptr;
	std::string p;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la)))
		p = narrow(la) + "\\Microsoft\\WindowsApps\\trackspin-agent.exe";
	CoTaskMemFree(la);
	return p;
}

// Fallback when the user turned the alias off in Settings: activate the package app directly.
// Returns a process handle or nullptr.
HANDLE activate_store_agent(const std::string &args)
{
	HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	HANDLE proc = nullptr;
	IApplicationActivationManager *mgr = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER,
				       IID_PPV_ARGS(&mgr)))) {
		DWORD pid = 0;
		std::wstring aumid = widen(g_store.family + "!Trackspin");
		if (SUCCEEDED(mgr->ActivateApplication(aumid.c_str(), widen(args).c_str(), AO_NOERRORUI, &pid)) && pid)
			proc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		mgr->Release();
	}
	if (init == S_OK || init == S_FALSE)
		CoUninitialize();
	return proc;
}

// Deletes the plugin folder after OBS exits. The files were created by this user, so no admin rights needed.
void schedule_self_removal()
{
	std::string dir = module_dir();
	if (!dir.empty())
		dir.pop_back();
	std::string quoted;
	for (char c : dir)
		quoted += c == '\'' ? std::string("''") : std::string(1, c);
	wchar_t sys[MAX_PATH] = {};
	GetSystemDirectoryW(sys, MAX_PATH);
	std::wstring cmd = L"\"" + std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive "
			   L"-WindowStyle Hidden -Command \"Wait-Process -Id " + std::to_wstring(GetCurrentProcessId()) +
			   L" -ErrorAction SilentlyContinue; Start-Sleep -Seconds 1; Remove-Item -LiteralPath '" + widen(quoted) +
			   L"' -Recurse -Force -ErrorAction SilentlyContinue\"";
	STARTUPINFOW si = {};
	si.cb = sizeof si;
	PROCESS_INFORMATION pi = {};
	DWORD flags = CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB;
	BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi);
	if (!ok) // the job may not allow breakaway
		ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	if (ok) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		obs_log(LOG_INFO, "plugin folder will be removed after OBS exits: %s", dir.c_str());
	} else {
		obs_log(LOG_WARNING, "cannot schedule plugin removal (error %lu)", GetLastError());
	}
}

std::string agent_path()
{
	if (g_store.store)
		return store_alias_path();
	return module_dir() + "trackspin-agent.exe";
}

// Строка JSON: кавычки и обратная косая экранируются, управляющие символы отбрасываются.
std::string json_string(const std::string &v)
{
	std::string out = "\"";
	for (unsigned char c : v) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += (char)c;
		} else if (c >= 0x20) {
			out += (char)c;
		}
	}
	return out + "\"";
}

std::string options_json(const RuntimeOptions &o)
{
	auto b = [](bool v) { return v ? "true" : "false"; };
	std::string j = "{";
	j += "\"yandex\":"; j += b(o.yandex); j += ",";
	j += "\"vk\":"; j += b(o.vk); j += ",";
	j += "\"spotify\":"; j += b(o.spotify); j += ",";
	j += "\"appleMusic\":"; j += b(o.appleMusic); j += ",";
	j += "\"youtubeMusic\":"; j += b(o.youtubeMusic); j += ",";
	j += "\"soundcloud\":"; j += b(o.soundcloud); j += ",";
	j += "\"deezer\":"; j += b(o.deezer); j += ",";
	j += "\"tidal\":"; j += b(o.tidal); j += ",";
	j += "\"amazonMusic\":"; j += b(o.amazonMusic); j += ",";
	j += "\"youtube\":"; j += b(o.youtube); j += ",";
	j += "\"otherPlayers\":"; j += b(o.otherPlayers); j += ",";
	j += "\"videoPlayers\":"; j += b(o.videoPlayers); j += ",";
	j += "\"browserWithoutExtension\":"; j += b(o.browser_without_extension); j += ",";
	j += "\"demo\":"; j += b(o.demo); j += ",";
	j += "\"blocklist\":[";
	for (size_t i = 0; i < o.blocklist.size(); ++i)
		j += (i ? "," : "") + json_string(o.blocklist[i]);
	j += "],\"outputDir\":" + json_string(o.output_dir);
	return j + "}";
}

int port_from_url(const std::string &url)
{
	size_t colon = url.rfind(':');
	return colon == std::string::npos ? 0 : std::atoi(url.c_str() + colon + 1);
}

bool post(const std::string &base, const std::string &token, const char *path, const std::string &body)
{
	if (base.empty())
		return false;
	httplib::Client cli("127.0.0.1", port_from_url(base));
	cli.set_connection_timeout(1, 0);
	cli.set_read_timeout(2, 0);
	httplib::Headers h = {{"X-Trackspin-Token", token}};
	auto res = cli.Post(path, h, body, "application/json");
	return res && res->status == 200;
}

// Запускает агент и ждёт, пока он сообщит порт. Вызывается под g_mu.
bool launch_locked()
{
	g_token = random_token();
	g_base_url.clear();
	std::remove(g_port_file.c_str());

	std::string exe = agent_path();
	std::string cmd = "\"" + exe + "\" --token " + g_token + " --port-file \"" + g_port_file + "\" --parent-pid " +
			  std::to_string(GetCurrentProcessId()) + " --log-file \"" + g_log_file + "\"";
	std::wstring wcmd = widen(cmd);
	STARTUPINFOW si = {};
	si.cb = sizeof si;
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
			    &pi)) {
		DWORD err = GetLastError();
		HANDLE h = nullptr;
		if (g_store.store) {
			std::string args = cmd.substr(exe.size() + 3); // drop the quoted exe path and the space
			h = activate_store_agent(args);
		}
		if (!h) {
			obs_log(LOG_ERROR, "cannot start agent %s (error %lu)", exe.c_str(), err);
			return false;
		}
		pi = {};
		pi.hProcess = h;
		pi.dwProcessId = GetProcessId(h);
	}
	g_proc = pi;

	for (int i = 0; i < 100; ++i) { // до 5 секунд
		std::ifstream f(g_port_file);
		int port = 0;
		if (f >> port && port > 0) {
			g_base_url = "http://127.0.0.1:" + std::to_string(port);
			obs_log(LOG_INFO, "agent started: %s/widget (pid %lu)", g_base_url.c_str(), pi.dwProcessId);
			post(g_base_url, g_token, "/api/config", options_json(g_opts));
			return true;
		}
		if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0)
			break;
	}
	obs_log(LOG_ERROR, "agent did not report a port; see %s", g_log_file.c_str());
	return false;
}

void close_handles_locked()
{
	if (g_proc.hProcess)
		CloseHandle(g_proc.hProcess);
	if (g_proc.hThread)
		CloseHandle(g_proc.hThread);
	g_proc = {};
}

// Microsoft Store build: after a Store update the agent replaces this plugin's files on disk and reports it in
// /api/health. Tell the user to restart OBS, but never in the middle of a stream or a recording.
void check_plugin_update()
{
	if (g_update_shown || !g_store.store)
		return;
	if (g_update_to.empty()) {
		obs_data_t *d = obs_data_create_from_json(runtime_get("/api/health").c_str());
		if (!d)
			return;
		const char *v = obs_data_get_string(d, "pluginUpdatedTo");
		if (v && *v && std::string(v) != PLUGIN_VERSION)
			g_update_to = v;
		obs_data_release(d);
		if (g_update_to.empty())
			return;
	}
	if (obs_frontend_streaming_active() || obs_frontend_recording_active() || obs_frontend_replay_buffer_active() ||
	    obs_frontend_virtualcam_active())
		return;
	// The agent draws the dialog (Windows 10/11 styling, dark mode); we pass the OBS theme, language and window.
	const char *locale = obs_get_locale();
	std::string body = "{\"kind\":\"update\",\"version\":" + json_string(g_update_to) + ",\"lang\":" +
			   json_string(locale ? locale : "en") + ",\"dark\":" +
			   (obs_frontend_is_theme_dark() ? "true" : "false") + ",\"owner\":" +
			   std::to_string(reinterpret_cast<uintptr_t>(obs_frontend_get_main_window_handle())) + "}";
	std::string base, token;
	{
		std::lock_guard<std::mutex> lk(g_mu);
		base = g_base_url;
		token = g_token;
	}
	if (post(base, token, "/api/notice", body))
		g_update_shown = true;
}

// Следит за агентом и перезапускает его при падении (не чаще 5 раз за минуту).
void watchdog()
{
	auto window_start = std::chrono::steady_clock::now();
	int restarts = 0;
	int tick = 0;
	while (!g_stopping) {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		if (++tick % 20 == 0) // every 10 s, outside the lock: it makes an HTTP request
			check_plugin_update();
		std::lock_guard<std::mutex> lk(g_mu);
		if (g_stopping)
			break;
		if (g_proc.hProcess && WaitForSingleObject(g_proc.hProcess, 0) == WAIT_TIMEOUT)
			continue;
		auto now = std::chrono::steady_clock::now();
		if (now - window_start > std::chrono::minutes(1)) {
			window_start = now;
			restarts = 0;
		}
		if (restarts >= 5)
			continue;
		++restarts;
		obs_log(LOG_WARNING, "agent is not running, restarting (%d)", restarts);
		close_handles_locked();
		launch_locked();
	}
}

} // namespace

bool runtime_start()
{
	// Leftover from a Store update that replaced a loaded DLL (agent/store_setup.cpp).
	DeleteFileW(widen(module_dir() + "trackspin.dll.old").c_str());
	g_store = read_store_marker();
	if (g_store.store && !package_installed(g_store.family)) {
		g_inactive = true;
		if (g_store.sid.empty() || g_store.sid == current_user_sid()) {
			g_remove_on_unload = true;
			obs_log(LOG_INFO, "Trackspin was uninstalled from the Microsoft Store; removing the plugin");
		} else {
			obs_log(LOG_INFO, "Trackspin from the Microsoft Store is installed for another Windows user; inactive");
		}
		return false;
	}

	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		std::string d = dir;
		bfree(dir);
		g_port_file = d + "agent-" + std::to_string(GetCurrentProcessId()) + ".port";
		g_log_file = d + "agent.log";
	}
	g_stopping = false;
	{
		std::lock_guard<std::mutex> lk(g_mu);
		launch_locked();
	}
	g_watchdog = std::thread(watchdog);
	return true;
}

void runtime_stop()
{
	if (g_inactive) {
		if (g_remove_on_unload)
			schedule_self_removal();
		return;
	}
	g_stopping = true;
	if (g_watchdog.joinable())
		g_watchdog.join();
	std::lock_guard<std::mutex> lk(g_mu);
	if (g_proc.hProcess) {
		post(g_base_url, g_token, "/api/shutdown", "");
		if (WaitForSingleObject(g_proc.hProcess, 2000) == WAIT_TIMEOUT)
			TerminateProcess(g_proc.hProcess, 0);
		close_handles_locked();
	}
	g_base_url.clear();
	std::remove(g_port_file.c_str());
}

void runtime_set_options(const RuntimeOptions &opts)
{
	std::string base, token, body;
	{
		std::lock_guard<std::mutex> lk(g_mu);
		g_opts = opts;
		base = g_base_url;
		token = g_token;
		body = options_json(opts);
	}
	// Не блокируем поток OBS сетевым запросом.
	std::thread([base, token, body] { post(base, token, "/api/config", body); }).detach();
}

std::string runtime_base_url()
{
	std::lock_guard<std::mutex> lk(g_mu);
	return g_base_url;
}

std::string runtime_get(const char *path)
{
	std::string base = runtime_base_url();
	if (base.empty())
		return std::string();
	httplib::Client cli("127.0.0.1", port_from_url(base));
	cli.set_connection_timeout(0, 300000);
	cli.set_read_timeout(0, 500000);
	auto res = cli.Get(path);
	return res && res->status == 200 ? res->body : std::string();
}

#else // не Windows: агент пока только для Windows

bool runtime_start()
{
	return true;
}
void runtime_stop() {}
void runtime_set_options(const RuntimeOptions &) {}
std::string runtime_base_url()
{
	return std::string();
}
std::string runtime_get(const char *)
{
	return std::string();
}

#endif

} // namespace trackspin
