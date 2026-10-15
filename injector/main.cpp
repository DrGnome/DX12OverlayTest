#include <Windows.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace
{
class Handle
{
public:
    explicit Handle(HANDLE value = nullptr) : m_value(value) {}
    ~Handle()
    {
        if (m_value && m_value != INVALID_HANDLE_VALUE)
        {
            CloseHandle(m_value);
        }
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    HANDLE Get() const
    {
        return m_value;
    }

private:
    HANDLE m_value;
};

std::wstring FormatWindowsError(DWORD error)
{
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&message), 0, nullptr);

    std::wstring result = length && message
        ? std::wstring(message, length)
        : L"Unknown Windows error";
    if (message)
    {
        LocalFree(message);
    }
    while (!result.empty() &&
           (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' '))
    {
        result.pop_back();
    }
    return result;
}

bool GetDefaultDllPath(std::wstring& path)
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            std::wcerr << L"Could not locate the injector executable: "
                       << FormatWindowsError(GetLastError()) << L"\n";
            return false;
        }
        if (length < buffer.size() - 1)
        {
            buffer.resize(length);
            break;
        }
        if (buffer.size() >= 32768)
        {
            std::wcerr << L"The injector executable path is too long.\n";
            return false;
        }
        buffer.resize(buffer.size() * 2);
    }

    const std::wstring executablePath(buffer.data(), buffer.size());
    const std::size_t separator = executablePath.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
    {
        std::wcerr << L"Could not determine the injector executable directory.\n";
        return false;
    }

    path.assign(executablePath, 0, separator + 1);
    path += L"DX12OverlayHook.dll";
    return true;
}

bool GetAbsoluteDllPath(const wchar_t* input, std::wstring& path)
{
    const DWORD required = GetFullPathNameW(input, 0, nullptr, nullptr);
    if (required == 0)
    {
        std::wcerr << L"Could not resolve DLL path: "
                   << FormatWindowsError(GetLastError()) << L"\n";
        return false;
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1);
    const DWORD length = GetFullPathNameW(
        input, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length == 0 || length >= buffer.size())
    {
        std::wcerr << L"Could not resolve DLL path: "
                   << FormatWindowsError(GetLastError()) << L"\n";
        return false;
    }
    path.assign(buffer.data(), length);

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES
            ? GetLastError()
            : ERROR_DIRECTORY;
        std::wcerr << L"DLL path is not a readable file: "
                   << FormatWindowsError(error) << L"\n";
        return false;
    }

    return true;
}

bool IsTargetAmd64(HANDLE process)
{
    using IsWow64Process2Function = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto isWow64Process2 = reinterpret_cast<IsWow64Process2Function>(
        GetProcAddress(kernel, "IsWow64Process2"));
    if (!isWow64Process2)
    {
        std::wcerr << L"IsWow64Process2 is unavailable; cannot safely verify "
                      L"the target architecture.\n";
        return false;
    }

    USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!isWow64Process2(process, &processMachine, &nativeMachine))
    {
        std::wcerr << L"Could not determine target architecture: "
                   << FormatWindowsError(GetLastError()) << L"\n";
        return false;
    }

    const USHORT targetMachine = processMachine == IMAGE_FILE_MACHINE_UNKNOWN
        ? nativeMachine
        : processMachine;
    if (targetMachine != IMAGE_FILE_MACHINE_AMD64)
    {
        std::wcerr << L"The injector and hook DLL are x64; the target process "
                      L"must also be x64.\n";
        return false;
    }
    return true;
}

int Inject(DWORD processId, const std::wstring& dllPath)
{
    Handle process(OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, processId));
    if (!process.Get())
    {
        std::wcerr << L"Could not open process " << processId << L": "
                   << FormatWindowsError(GetLastError()) << L"\n";
        return 1;
    }

    if (!IsTargetAmd64(process.Get()))
    {
        return 1;
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibrary = GetProcAddress(kernel, "LoadLibraryW");
    if (!loadLibrary)
    {
        std::wcerr << L"Could not find LoadLibraryW: "
                   << FormatWindowsError(GetLastError()) << L"\n";
        return 1;
    }

    const SIZE_T pathSize = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(
        process.Get(), nullptr, pathSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remotePath)
    {
        std::wcerr << L"Could not allocate memory in target process: "
                   << FormatWindowsError(GetLastError()) << L"\n";
        return 1;
    }

    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(
            process.Get(), remotePath, dllPath.c_str(), pathSize, &bytesWritten) ||
        bytesWritten != pathSize)
    {
        const DWORD error = GetLastError();
        VirtualFreeEx(process.Get(), remotePath, 0, MEM_RELEASE);
        std::wcerr << L"Could not write the DLL path into target process: "
                   << FormatWindowsError(error) << L"\n";
        return 1;
    }

    Handle thread(CreateRemoteThread(
        process.Get(), nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary),
        remotePath, 0, nullptr));
    if (!thread.Get())
    {
        const DWORD error = GetLastError();
        VirtualFreeEx(process.Get(), remotePath, 0, MEM_RELEASE);
        std::wcerr << L"Could not start the remote loader thread: "
                   << FormatWindowsError(error) << L"\n";
        return 1;
    }

    constexpr DWORD WaitTimeoutMilliseconds = 30000;
    const DWORD waitResult = WaitForSingleObject(
        thread.Get(), WaitTimeoutMilliseconds);
    if (waitResult != WAIT_OBJECT_0)
    {
        if (waitResult == WAIT_TIMEOUT)
        {
            std::wcerr << L"Remote DLL load did not finish within "
                       << WaitTimeoutMilliseconds
                       << L" ms. The remote path buffer was left allocated "
                          L"because the thread may still be using it.\n";
        }
        else
        {
            std::wcerr << L"Waiting for the remote loader thread failed: "
                       << FormatWindowsError(GetLastError())
                       << L" The remote path buffer was left allocated because "
                          L"the thread may still be using it.\n";
        }
        return 1;
    }

    DWORD remoteResult = 0;
    if (!GetExitCodeThread(thread.Get(), &remoteResult))
    {
        const DWORD error = GetLastError();
        VirtualFreeEx(process.Get(), remotePath, 0, MEM_RELEASE);
        std::wcerr << L"Could not read the remote loader result: "
                   << FormatWindowsError(error) << L"\n";
        return 1;
    }
    VirtualFreeEx(process.Get(), remotePath, 0, MEM_RELEASE);

    if (remoteResult == 0)
    {
        std::wcerr << L"LoadLibraryW failed in the target process. Check that "
                      L"the DLL and its dependencies are accessible there.\n";
        return 1;
    }

    std::wcout << L"Loaded " << dllPath << L" into process " << processId
               << L". The hook installs asynchronously from the DLL's worker thread.\n";
    return 0;
}

struct WindowSearch
{
    const wchar_t* title;
    std::vector<DWORD> processIds;
};

BOOL CALLBACK FindWindowByTitle(HWND window, LPARAM parameter)
{
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    const int titleLength = GetWindowTextLengthW(window);
    if (titleLength <= 0)
    {
        return TRUE;
    }

    std::vector<wchar_t> title(static_cast<std::size_t>(titleLength) + 1);
    const int copiedLength = GetWindowTextW(
        window, title.data(), static_cast<int>(title.size()));
    if (copiedLength != titleLength ||
        std::wstring(title.data(), copiedLength) != search.title)
    {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != 0 &&
        std::find(search.processIds.begin(), search.processIds.end(), processId) ==
            search.processIds.end())
    {
        search.processIds.push_back(processId);
    }
    return TRUE;
}

bool FindTargetProcess(const wchar_t* windowTitle, DWORD& processId)
{
    WindowSearch search{windowTitle, {}};
    if (!EnumWindows(FindWindowByTitle, reinterpret_cast<LPARAM>(&search)))
    {
        std::wcerr << L"Could not enumerate top-level windows: "
                   << FormatWindowsError(GetLastError()) << L"\n";
        return false;
    }

    if (search.processIds.empty())
    {
        std::wcerr << L"Could not find a top-level window titled \""
                   << windowTitle << L"\". Start the application and use its "
                                      L"exact window title.\n";
        return false;
    }
    if (search.processIds.size() > 1)
    {
        std::wcerr << L"More than one process has a top-level window titled \""
                   << windowTitle << L"\". Close duplicate instances and retry. "
                                      L"Matching process IDs:";
        for (const DWORD matchingProcessId : search.processIds)
        {
            std::wcerr << L" " << matchingProcessId;
        }
        std::wcerr << L"\n";
        return false;
    }

    processId = search.processIds.front();
    return true;
}
} // namespace

int wmain(int argc, wchar_t* argv[])
{
    if (argc != 2 && argc != 3)
    {
        std::wcerr << L"Usage: DX12OverlayInjector.exe <window-title> [path-to-dll]\n"
                      L"If path-to-dll is omitted, DX12OverlayHook.dll is loaded "
                      L"from the injector's directory.\n";
        return 2;
    }

    DWORD processId = 0;
    if (!FindTargetProcess(argv[1], processId))
    {
        return 2;
    }

    std::wstring inputDllPath;
    if (argc == 3)
    {
        inputDllPath = argv[2];
    }
    else if (!GetDefaultDllPath(inputDllPath))
    {
        return 2;
    }

    std::wstring dllPath;
    if (!GetAbsoluteDllPath(inputDllPath.c_str(), dllPath))
    {
        return 2;
    }

    return Inject(processId, dllPath);
}
