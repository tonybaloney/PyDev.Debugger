/* ****************************************************************************
*
* Copyright (c) Microsoft Corporation.
*
* This source code is subject to terms and conditions of the Apache License, Version 2.0. A
* copy of the license can be found in the License.html file at the root of this distribution. If
* you cannot locate the Apache License, Version 2.0, please send an email to
* vspython@microsoft.com. By using this source code in any fashion, you are agreeing to be bound
* by the terms of the Apache License, Version 2.0.
*
* You must not remove this notice, or any other, from this software.
*
* Contributor: Fabio Zadrozny
*
* Based on PyDebugAttach.cpp from PVTS. Windows only.
*
* https://github.com/Microsoft/PTVS/blob/master/Python/Product/PyDebugAttach/PyDebugAttach.cpp
*
* Initially we did an attach completely based on shellcode which got the
* GIL called PyRun_SimpleString with the needed code and was done with it
* (so, none of this code was needed).
* Now, newer version of Python don't initialize threading by default, so,
* most of this code is done only to overcome this limitation (and as a plus,
* if there's no code running, we also pause the threads to make our code run).
*
* On Linux the approach is still the simpler one (using gdb), so, on newer
* versions of Python it may not work unless the user has some code running
* and threads are initialized.
* I.e.:
*
* The user may have to add the code below in the start of its script for
* a successful attach (if he doesn't already use threads).
*
* from threading import Thread
* Thread(target=str).start()
*
* -- this is the workaround for the fact that we can't get the gil
* if there aren't any threads (PyGILState_Ensure gives an error).
* ***************************************************************************/


// Access to std::cout and std::endl
#include <iostream>

// DECLDIR will perform an export for us
#define DLL_EXPORT

#include "attach.h"
#include "stdafx.h"

#include "../common/python.h"
#include "../common/ref_utils.hpp"
#include "../common/py_utils.hpp"
#include "../common/py_settrace.hpp"


#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")

// _Always_ is not defined for all versions, so make it a no-op if missing.
#ifndef _Always_
#define _Always_(x) x
#endif


typedef void (PyEval_Lock)(); // Acquire/Release lock
typedef void (PyThreadState_API)(PyThreadState *); // Acquire/Release lock
typedef PyObject* (Py_CompileString)(const char *str, const char *filename, int start);
typedef PyObject* (PyEval_EvalCode)(PyObject *co, PyObject *globals, PyObject *locals);
typedef PyObject* (PyDict_GetItemString)(PyObject *p, const char *key);
typedef PyObject* (PyEval_GetBuiltins)();
typedef int (PyDict_SetItemString)(PyObject *dp, const char *key, PyObject *item);
typedef int (PyEval_ThreadsInitialized)();
typedef int (Py_AddPendingCall)(int (*func)(void *), void*);
typedef PyObject* (PyString_FromString)(const char* s);
typedef void PyEval_SetTrace(Py_tracefunc func, PyObject *obj);
typedef PyObject* (PyErr_Print)();
typedef PyObject* (PyObject_SetAttrString)(PyObject *o, const char *attr_name, PyObject* value);
typedef PyObject* (PyBool_FromLong)(long v);
typedef unsigned long (_PyEval_GetSwitchInterval)(void);
typedef void (_PyEval_SetSwitchInterval)(unsigned long microseconds);
typedef PyGILState_STATE PyGILState_EnsureFunc(void);
typedef void PyGILState_ReleaseFunc(PyGILState_STATE);
typedef PyThreadState *PyThreadState_NewFunc(PyInterpreterState *interp);

typedef PyObject *PyList_New(Py_ssize_t len);
typedef int PyList_Append(PyObject *list, PyObject *item);



std::wstring GetCurrentModuleFilename() {
    HMODULE hModule = nullptr;
    if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCTSTR)GetCurrentModuleFilename, &hModule) != 0) {
        wchar_t filename[MAX_PATH];
        GetModuleFileName(hModule, filename, MAX_PATH);
        return filename;
    }
    return std::wstring();
}


struct InitializeThreadingInfo {
    PyImport_ImportModule* pyImportMod;
    PyEval_Lock* initThreads;
    HANDLE event;
};

HANDLE g_initedEvent;
int AttachCallback(void *voidInitializeThreadingInfo) {
    // initialize us for threading, this will acquire the GIL if not already created, and is a nop if the GIL is created.
    // This leaves us in the proper state when we return back to the runtime whether the GIL was created or not before
    // we were called.
    InitializeThreadingInfo* initializeThreadingInfo = (InitializeThreadingInfo*)voidInitializeThreadingInfo;
    initializeThreadingInfo->initThreads();
    std::cout << "AttachCallback: initThreads" << std::endl << std::flush;
    initializeThreadingInfo->pyImportMod("threading");
    std::cout << "AttachCallback: imported threading" << std::endl << std::flush;
    SetEvent(g_initedEvent);
    return 0;
}


char* ReadCodeFromFile(wchar_t* filePath) {
    std::ifstream filestr;
    filestr.open(filePath, std::ios::binary);
    if (filestr.fail()) {
        return nullptr;
    }

    // get length of file:
    filestr.seekg(0, std::ios::end);
    auto length = filestr.tellg();
    filestr.seekg(0, std::ios::beg);

    int len = (int)length;
    char* buffer = new char[len + 1];
    filestr.read(buffer, len);
    buffer[len] = 0;

    // remove carriage returns, copy zero byte
    for (int read = 0, write = 0; read <= len; read++) {
        if (buffer[read] == '\r') {
            continue;
        } else if (write != read) {
            buffer[write] = buffer[read];
        }
        write++;
    }

    return buffer;
}

// create a custom heap for our unordered map.  This is necessary because if we suspend a thread while in a heap function
// then we could deadlock here.  We need to be VERY careful about what we do while the threads are suspended.
static HANDLE g_heap = 0;

template<typename T>
class PrivateHeapAllocator {
public:
    typedef size_t    size_type;
    typedef ptrdiff_t difference_type;
    typedef T*        pointer;
    typedef const T*  const_pointer;
    typedef T&        reference;
    typedef const T&  const_reference;
    typedef T         value_type;

    template<class U>
    struct rebind {
        typedef PrivateHeapAllocator<U> other;
    };

    explicit PrivateHeapAllocator() {}

    PrivateHeapAllocator(PrivateHeapAllocator const&) {}

    ~PrivateHeapAllocator() {}

    template<typename U>
    PrivateHeapAllocator(PrivateHeapAllocator<U> const&) {}

    pointer allocate(size_type size, std::allocator<void>::const_pointer hint = 0) {
        UNREFERENCED_PARAMETER(hint);

        if (g_heap == nullptr) {
            g_heap = HeapCreate(0, 0, 0);
        }
        auto mem = HeapAlloc(g_heap, 0, size * sizeof(T));
        return static_cast<pointer>(mem);
    }

    void deallocate(pointer p, size_type n) {
        UNREFERENCED_PARAMETER(n);

        HeapFree(g_heap, 0, p);
    }

    size_type max_size() const {
        return (std::numeric_limits<size_type>::max)() / sizeof(T);
    }

    void construct(pointer p, const T& t) {
        new(p) T(t);
    }

    void destroy(pointer p) {
        p->~T();
    }
};

typedef std::unordered_map<DWORD, HANDLE, std::hash<DWORD>, std::equal_to<DWORD>, PrivateHeapAllocator<std::pair<DWORD, HANDLE>>> ThreadMap;

void ResumeThreads(ThreadMap &suspendedThreads) {
    for (auto start = suspendedThreads.begin();  start != suspendedThreads.end(); start++) {
        ResumeThread((*start).second);
        CloseHandle((*start).second);
    }
    suspendedThreads.clear();
}

// Suspends all threads ensuring that they are not currently in a call to Py_AddPendingCall.
void SuspendThreads(ThreadMap &suspendedThreads, Py_AddPendingCall* addPendingCall, PyEval_ThreadsInitialized* threadsInited) {
    DWORD curThreadId = GetCurrentThreadId();
    DWORD curProcess = GetCurrentProcessId();
    // suspend all the threads in the process so we can do things safely...
    bool suspended;

    do {
        suspended = false;
        HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (h != INVALID_HANDLE_VALUE) {

            THREADENTRY32 te;
            te.dwSize = sizeof(te);
            if (Thread32First(h, &te)) {
                do {
                    if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID) && te.th32OwnerProcessID == curProcess) {


                        if (te.th32ThreadID != curThreadId && suspendedThreads.find(te.th32ThreadID) == suspendedThreads.end()) {
                            auto hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
                            if (hThread != nullptr) {
                                SuspendThread(hThread);

                                bool addingPendingCall = false;

                                CONTEXT context;
                                memset(&context, 0x00, sizeof(CONTEXT));
                                context.ContextFlags = CONTEXT_ALL;
                                GetThreadContext(hThread, &context);

#if defined(_X86_)
                                if (context.Eip >= *((DWORD*)addPendingCall) && context.Eip <= (*((DWORD*)addPendingCall)) + 0x100) {
                                    addingPendingCall = true;
                                }
#elif defined(_AMD64_)
                                if (context.Rip >= *((DWORD64*)addPendingCall) && context.Rip <= *((DWORD64*)addPendingCall + 0x100)) {
                                    addingPendingCall = true;
                                }
#endif

                                if (addingPendingCall) {
                                    // we appear to be adding a pending call via this thread - wait for this to finish so we can add our own pending call...
                                    ResumeThread(hThread);
                                    SwitchToThread();   // yield to the resumed thread if it's on our CPU...
                                    CloseHandle(hThread);
                                } else {
                                    suspendedThreads[te.th32ThreadID] = hThread;
                                }
                                suspended = true;
                            }
                        }
                    }

                    te.dwSize = sizeof(te);
                } while (Thread32Next(h, &te) && !threadsInited());
            }
            CloseHandle(h);
        }
    } while (suspended && !threadsInited());
}


// Ensures handles are closed when they go out of scope
class HandleHolder {
    HANDLE _handle;
public:
    HandleHolder(HANDLE handle) : _handle(handle) {
    }

    ~HandleHolder() {
        CloseHandle(_handle);
    }
};




bool LoadAndEvaluateCode(
    wchar_t* filePath, const char* fileName, bool isDebug, PyObject* globalsDict,
    Py_CompileString* pyCompileString, PyDict_SetItemString* dictSetItem,
    PyEval_EvalCode* pyEvalCode, PyString_FromString* strFromString, PyEval_GetBuiltins* getBuiltins,
    PyErr_Print pyErrPrint
 ) {
    auto debuggerCode = ReadCodeFromFile(filePath);
    if (debuggerCode == nullptr) {
        return false;
    }

    auto code = PyObjectHolder(isDebug, pyCompileString(debuggerCode, fileName, 257 /*Py_file_input*/));
    delete[] debuggerCode;

    if (*code == nullptr) {
        return false;
    }

    dictSetItem(globalsDict, "__builtins__", getBuiltins());
    auto size = WideCharToMultiByte(CP_UTF8, 0, filePath, (DWORD)wcslen(filePath), nullptr, 0, nullptr, nullptr);
    char* filenameBuffer = new char[size + 1];
    if (WideCharToMultiByte(CP_UTF8, 0, filePath, (DWORD)wcslen(filePath), filenameBuffer, size, nullptr, nullptr) != 0) {
        filenameBuffer[size] = 0;
        dictSetItem(globalsDict, "__file__", strFromString(filenameBuffer));
    }

    auto evalResult = PyObjectHolder(isDebug, pyEvalCode(code.ToPython(), globalsDict, globalsDict));
#if !NDEBUG
    if (*evalResult == nullptr) {
        pyErrPrint();
    }
#else
    UNREFERENCED_PARAMETER(pyErrPrint);
#endif

    return true;
}

// Checks to see if the specified module is likely a Python interpreter.
bool IsPythonModule(HMODULE module, bool &isDebug) {
    wchar_t mod_name[MAX_PATH];
    isDebug = false;
    if (GetModuleBaseName(GetCurrentProcess(), module, mod_name, MAX_PATH)) {
        if (_wcsnicmp(mod_name, L"python", 6) == 0) {
            if (wcslen(mod_name) >= 10 && _wcsnicmp(mod_name + 8, L"_d", 2) == 0) {
                isDebug = true;
            }
            
            // Check if the module has Py_IsInitialized.
            DEFINE_PROC_NO_CHECK(isInit, Py_IsInitialized*, "Py_IsInitialized", 0);
            DEFINE_PROC_NO_CHECK(gilEnsure, PyGILState_Ensure*, "PyGILState_Ensure", 51);
            DEFINE_PROC_NO_CHECK(gilRelease, PyGILState_Release*, "PyGILState_Release", 51);
            if (isInit == nullptr || gilEnsure == nullptr || gilRelease == nullptr) {
                return false;
            }
            

            return true;
        }
    }
    return false;
}

extern "C"
{

    /**
     * The returned value signals the error that happened!
     *
     * Return codes:
     * 0 = all OK.
     * 1 = Py_IsInitialized not found
     * 2 = Py_IsInitialized returned false
     * 3 = Missing Python API
     * 4 = Interpreter not initialized
     * 5 = Python version unknown
     * 6 = Connect timeout
     **/
	int DoAttach(HMODULE module, bool isDebug, const char *command, bool showDebugInfo )
	{
	   showDebugInfo = true;
        auto isInit = (Py_IsInitialized*)GetProcAddress(module, "Py_IsInitialized");

        if (isInit == nullptr) {
            if (showDebugInfo) {
                std::cout << "Py_IsInitialized not found. " << std::endl << std::flush;
            }
            return 1;
        }
        if (!isInit()) {
            if (showDebugInfo) {
                std::cout << "Py_IsInitialized returned false. " << std::endl << std::flush;
            }
            return 2;
        }

        auto version = GetPythonVersion(module);

        // found initialized Python runtime, gather and check the APIs we need for a successful attach...
        auto addPendingCall = (Py_AddPendingCall*)GetProcAddress(module, "Py_AddPendingCall");
        auto interpHead = (PyInterpreterState_Head*)GetProcAddress(module, "PyInterpreterState_Head");
        auto gilEnsure = (PyGILState_Ensure*)GetProcAddress(module, "PyGILState_Ensure");
        auto gilRelease = (PyGILState_Release*)GetProcAddress(module, "PyGILState_Release");
        auto threadHead = (PyInterpreterState_ThreadHead*)GetProcAddress(module, "PyInterpreterState_ThreadHead");
        auto initThreads = (PyEval_Lock*)GetProcAddress(module, "PyEval_InitThreads");
        auto releaseLock = (PyEval_Lock*)GetProcAddress(module, "PyEval_ReleaseLock");
        auto threadsInited = (PyEval_ThreadsInitialized*)GetProcAddress(module, "PyEval_ThreadsInitialized");
        auto threadNext = (PyThreadState_Next*)GetProcAddress(module, "PyThreadState_Next");
        auto threadSwap = (PyThreadState_Swap*)GetProcAddress(module, "PyThreadState_Swap");
        auto pyCompileString = (Py_CompileString*)GetProcAddress(module, "Py_CompileString");
        auto pyEvalCode = (PyEval_EvalCode*)GetProcAddress(module, "PyEval_EvalCode");
        auto getDictItem = (PyDict_GetItemString*)GetProcAddress(module, "PyDict_GetItemString");
        auto call = (PyObject_CallFunctionObjArgs*)GetProcAddress(module, "PyObject_CallFunctionObjArgs");
        auto getBuiltins = (PyEval_GetBuiltins*)GetProcAddress(module, "PyEval_GetBuiltins");
        auto dictSetItem = (PyDict_SetItemString*)GetProcAddress(module, "PyDict_SetItemString");
        PyInt_FromLong* intFromLong;
        PyString_FromString* strFromString;
        if (version >= PythonVersion_30) {
            intFromLong = (PyInt_FromLong*)GetProcAddress(module, "PyLong_FromLong");
            if (version >= PythonVersion_33) {
                strFromString = (PyString_FromString*)GetProcAddress(module, "PyUnicode_FromString");
            } else {
                strFromString = (PyString_FromString*)GetProcAddress(module, "PyUnicodeUCS2_FromString");
            }
        } else {
            intFromLong = (PyInt_FromLong*)GetProcAddress(module, "PyInt_FromLong");
            strFromString = (PyString_FromString*)GetProcAddress(module, "PyString_FromString");
        }
        auto errOccurred = (PyErr_Occurred*)GetProcAddress(module, "PyErr_Occurred");
        auto pyErrFetch = (PyErr_Fetch*)GetProcAddress(module, "PyErr_Fetch");
        auto pyErrRestore = (PyErr_Restore*)GetProcAddress(module, "PyErr_Restore");
        auto pyImportMod = (PyImport_ImportModule*) GetProcAddress(module, "PyImport_ImportModule");
        auto pyGetAttr = (PyObject_GetAttrString*)GetProcAddress(module, "PyObject_GetAttrString");
        auto pySetAttr = (PyObject_SetAttrString*)GetProcAddress(module, "PyObject_SetAttrString");
        auto pyNone = (PyObject*)GetProcAddress(module, "_Py_NoneStruct");

        auto getThreadTls = (PyThread_get_key_value*)GetProcAddress(module, "PyThread_get_key_value");
        auto setThreadTls = (PyThread_set_key_value*)GetProcAddress(module, "PyThread_set_key_value");
        auto delThreadTls = (PyThread_delete_key_value*)GetProcAddress(module, "PyThread_delete_key_value");
        auto pyRun_SimpleString = (PyRun_SimpleString*)GetProcAddress(module, "PyRun_SimpleString");

        // Either _PyThreadState_Current or _PyThreadState_UncheckedGet are required
        auto curPythonThread = (PyThreadState**)(void*)GetProcAddress(module, "_PyThreadState_Current");
        auto getPythonThread = (_PyThreadState_UncheckedGet*)GetProcAddress(module, "_PyThreadState_UncheckedGet");

        // Either _Py_CheckInterval or _PyEval_[GS]etSwitchInterval are useful, but not required
        auto intervalCheck = (int*)GetProcAddress(module, "_Py_CheckInterval");
        auto getSwitchInterval = (_PyEval_GetSwitchInterval*)GetProcAddress(module, "_PyEval_GetSwitchInterval");
        auto setSwitchInterval = (_PyEval_SetSwitchInterval*)GetProcAddress(module, "_PyEval_SetSwitchInterval");

        if (addPendingCall == nullptr || interpHead == nullptr || gilEnsure == nullptr || gilRelease == nullptr || threadHead == nullptr ||
            initThreads == nullptr || releaseLock == nullptr || threadsInited == nullptr || threadNext == nullptr || threadSwap == nullptr ||
            pyCompileString == nullptr || pyEvalCode == nullptr || getDictItem == nullptr || call == nullptr ||
            getBuiltins == nullptr || dictSetItem == nullptr || intFromLong == nullptr || pyErrRestore == nullptr || pyErrFetch == nullptr ||
            errOccurred == nullptr || pyImportMod == nullptr || pyGetAttr == nullptr || pyNone == nullptr || pySetAttr == nullptr || 
            getThreadTls == nullptr || setThreadTls == nullptr || delThreadTls == nullptr || 
            pyRun_SimpleString == nullptr ||
            (curPythonThread == nullptr && getPythonThread == nullptr)) {
                // we're missing some APIs, we cannot attach.
                if (showDebugInfo) {
                    std::cout << "Error, missing Python API!! " << std::endl << std::flush;
                }
                return 3;
        }

        auto head = interpHead();
        if (head == nullptr) {
            // this interpreter is loaded but not initialized.
            if (showDebugInfo) {
                std::cout << "Interpreter not initialized! " << std::endl << std::flush;
            }
            return 4;
        }

        // check that we're a supported version
        if (version == PythonVersion_Unknown) {
            if (showDebugInfo) {
                std::cout << "Python version unknown! " << std::endl << std::flush;
            }
            return 5;
        } else if (version == PythonVersion_25 || version == PythonVersion_26 ||
                   version == PythonVersion_30 || version == PythonVersion_31 || version == PythonVersion_32) {
            if (showDebugInfo) {
                std::cout << "Python version unsupported! " << std::endl << std::flush;
            }
            return 5;
        }


        std::cout << "threadsInited(): " << threadsInited() << std::endl << std::flush;
        if (threadsInited()) {
            // Note that since Python 3.7, threads are *always* initialized!
            if (showDebugInfo) {
                std::cout << "Threads already initialized! " << std::endl << std::flush;
            }

        } else {
            int saveIntervalCheck;
            unsigned long saveLongIntervalCheck;
            if (intervalCheck != nullptr) {
                // not available on 3.2
                saveIntervalCheck = *intervalCheck;
                *intervalCheck = -1;    // lower the interval check so pending calls are processed faster
                saveLongIntervalCheck = 0; // prevent compiler warning
            } else if (getSwitchInterval != nullptr && setSwitchInterval != nullptr) {
                saveLongIntervalCheck = getSwitchInterval();
                setSwitchInterval(0);
                saveIntervalCheck = 0; // prevent compiler warning
            }
            else {
                saveIntervalCheck = 0; // prevent compiler warning
                saveLongIntervalCheck = 0; // prevent compiler warning
            }

            // Multiple thread support has not been initialized in the interpreter.   We need multi threading support
            // to block any actively running threads and setup the debugger attach state.
            //
            // We need to initialize multiple threading support but we need to do so safely, so we call
            // Py_AddPendingCall and have our callback then initialize multi threading.  This is completely safe on 2.7
            // and up.  Unfortunately that doesn't work if we're not actively running code on the main thread (blocked on a lock
            // or reading input).
            //
            // Another option is to make sure no code is running - if there is no active thread then we can safely call
            // PyEval_InitThreads and we're in business.  But to know this is safe we need to first suspend all the other
            // threads in the process and then inspect if any code is running (note that this is still not ideal because
            // this thread will be the thread head for Python, but still better than not attach at all).
            //
            // Finally if code is running after we've suspended the threads then we can go ahead and do Py_AddPendingCall
            // on down-level interpreters as long as we're sure no one else is making a call to Py_AddPendingCall at the same
            // time.
            //
            // Therefore our strategy becomes: Make the Py_AddPendingCall on interpreters and wait for it. If it doesn't 
            // call after a timeout, suspend all threads - if a threads is in Py_AddPendingCall resume and try again.  Once we've got all of the threads
            // stopped and not in Py_AddPendingCall (which calls no functions its self, you can see this and it's size in the
            // debugger) then see if we have a current thread.   If not go ahead and initialize multiple threading (it's now safe,
            // no Python code is running). 
            //
            // If at any point during this process threading becomes initialized (due to our pending call 
            // or the Python code creating a new thread)  then we're done and we just resume all of the presently suspended threads.

            g_initedEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            HandleHolder holder(g_initedEvent);
            
            // Note that this will leak if the addPendingCall is not completed (when it completes
            // it suicides itself).
            InitializeThreadingInfo *initializeThreadingInfo = new InitializeThreadingInfo();
            initializeThreadingInfo->pyImportMod = pyImportMod;
            initializeThreadingInfo->initThreads = initThreads;

            // we're on a thread safe Python version, go ahead and pend our call to initialize threading.
            addPendingCall(&AttachCallback, initializeThreadingInfo);
            
            ::WaitForSingleObject(g_initedEvent, 5000);
            
            // If threads weren't initialized in our pending call, instead of giving a timeout, try
            // to initialize it in this thread.
            for(int attempts = 0; !threadsInited() && attempts < 20; attempts++) {
                if(attempts > 0){
                    // If we haven't been able to do it in the first time, wait a bit before retrying.
                    Sleep(10);
                }

                ThreadMap suspendedThreads;
                std::cout << "SuspendThreads(suspendedThreads, addPendingCall, threadsInited);" << std::endl << std::flush;
                SuspendThreads(suspendedThreads, addPendingCall, threadsInited);
                
                if(!threadsInited()){ // Check again with threads suspended.
                    std::cout << "ENTERED if (!threadsInited()) {" << std::endl << std::flush;
                    auto curPyThread = getPythonThread ? getPythonThread() : *curPythonThread;
                    
                    if (curPyThread == nullptr) {
                        std::cout << "ENTERED if (curPyThread == nullptr) {" << std::endl << std::flush;
                         // no threads are currently running, it is safe to initialize multi threading.
                         PyGILState_STATE gilState;
                         if (version >= PythonVersion_34) {
                             // in 3.4 due to http://bugs.python.org/issue20891,
                             // we need to create our thread state manually
                             // before we can call PyGILState_Ensure() before we
                             // can call PyEval_InitThreads().
    
                             // Don't require this function unless we need it.
                             auto threadNew = (PyThreadState_NewFunc*)GetProcAddress(module, "PyThreadState_New");
                             if (threadNew != nullptr) {
                                 threadNew(head);
                             }
                         }
    
                         if (version >= PythonVersion_32) {
                             // in 3.2 due to the new GIL and later we can't call Py_InitThreads
                             // without a thread being initialized.
                             // So we use PyGilState_Ensure here to first
                             // initialize the current thread, and then we use
                             // Py_InitThreads to bring up multi-threading.
                             // Some context here: http://bugs.python.org/issue11329
                             // http://pytools.codeplex.com/workitem/834
                            gilState = gilEnsure();
                        }
                        else {
                            gilState = PyGILState_LOCKED; // prevent compiler warning
                         }
    
                        std::cout << "Called initThreads()" << std::endl << std::flush;
                        // Initialize threads in our secondary thread (this is NOT ideal because
                        // this thread will be the thread head), but is still better than not being
                        // able to attach if the main thread is not actually running any code.
                        initThreads();
    
                         if (version >= PythonVersion_32) {
                             // we will release the GIL here
                            gilRelease(gilState);
                         } else {
                             releaseLock();
                         }
                    }
                } 
                ResumeThreads(suspendedThreads);
            }


            if (intervalCheck != nullptr) {
                *intervalCheck = saveIntervalCheck;
            } else if (setSwitchInterval != nullptr) {
                setSwitchInterval(saveLongIntervalCheck);
            }
            
        }

        if (g_heap != nullptr) {
            HeapDestroy(g_heap);
            g_heap = nullptr;
        }
        
        if (!threadsInited()) {
            std::cout << "Unable to initialize threads in the given timeout! " << std::endl << std::flush;
            return 8;
        }

        GilHolder gilLock(gilEnsure, gilRelease);   // acquire and hold the GIL until done...
        
        pyRun_SimpleString(command);
        return 0;

    }




    // ======================================== Code related to setting tracing to existing threads.
    
    struct ModuleInfo {
        HMODULE module;
        bool isDebug;
        int errorGettingModule; // 0 means ok, negative values some error (should never be positive).
    };
    

    ModuleInfo GetPythonModule() {
        HANDLE hProcess = GetCurrentProcess();
        ModuleInfo moduleInfo;
        moduleInfo.module = nullptr;
        moduleInfo.isDebug = false;
        moduleInfo.errorGettingModule = 0;
        
        DWORD modSize = sizeof(HMODULE) * 1024;
        HMODULE* hMods = (HMODULE*)_malloca(modSize);
        if (hMods == nullptr) {
            std::cout << "hmods not allocated! " << std::endl << std::flush;
            moduleInfo.errorGettingModule = -1;
            return moduleInfo;
        }

        DWORD modsNeeded;
        while (!EnumProcessModules(hProcess, hMods, modSize, &modsNeeded)) {
            // try again w/ more space...
            _freea(hMods);
            hMods = (HMODULE*)_malloca(modsNeeded);
            if (hMods == nullptr) {
                std::cout << "hmods not allocated (2)! " << std::endl << std::flush;
                moduleInfo.errorGettingModule = -2;
                return moduleInfo;
            }
            modSize = modsNeeded;
        }
        
        for (size_t i = 0; i < modsNeeded / sizeof(HMODULE); i++) {
            bool isDebug;
            if (IsPythonModule(hMods[i], isDebug)) {
                moduleInfo.isDebug = isDebug;
                moduleInfo.module = hMods[i]; 
                return moduleInfo;
            }
        }
        std::cout << "Unable to find python module. " << std::endl << std::flush;
        moduleInfo.errorGettingModule = -3;
        return moduleInfo;
    }
    

    /**
     * This function is meant to be called to execute some arbitrary python code to be
     * run. It'll initialize threads as needed and then run the code with pyRun_SimpleString.  
     *
     * @param command: the python code to be run
     * @param attachInfo: pointer to an int specifying whether we should show debug info (1) or not (0).
     **/
    DECLDIR int AttachAndRunPythonCode(const char *command, int *attachInfo )
    {
        
        int SHOW_DEBUG_INFO = 1;
        
        bool showDebugInfo = (*attachInfo & SHOW_DEBUG_INFO) != 0;
        
        if (showDebugInfo) {
            std::cout << "AttachAndRunPythonCode started (showing debug info). " << std::endl << std::flush;
        }
        
        ModuleInfo moduleInfo = GetPythonModule();
        if (moduleInfo.errorGettingModule != 0) {
            return moduleInfo.errorGettingModule;
        }
        HMODULE module = moduleInfo.module;
        int attached = DoAttach(module, moduleInfo.isDebug, command, showDebugInfo);
        
        if (attached != 0 && showDebugInfo) {
            std::cout << "Error when injecting code in target process. Error code (on windows): " << attached << std::endl << std::flush;
        }
        return attached;
    }
    


    /**
     * A negative value means there was some error getting it.
     */
    DECLDIR int GetMainThreadId() {
        ModuleInfo moduleInfo = GetPythonModule();
        if (moduleInfo.errorGettingModule != 0) {
            return moduleInfo.errorGettingModule;
        }
        HMODULE module = moduleInfo.module;
        
        DEFINE_PROC(interpHead, PyInterpreterState_Head*, "PyInterpreterState_Head", -100);
        DEFINE_PROC(threadHead, PyInterpreterState_ThreadHead*, "PyInterpreterState_ThreadHead", -110);
        
        auto head = interpHead();
        if (head == nullptr) {
            // this interpreter is loaded but not initialized.
            PRINT("Interpreter not initialized!");
            return -120;
        }
        
        auto version = GetPythonVersion(module);
        DEFINE_PROC(gilEnsure, PyGILState_Ensure*, "PyGILState_Ensure", -130);
        DEFINE_PROC(gilRelease, PyGILState_Release*, "PyGILState_Release", -140);

        GilHolder gilLock(gilEnsure, gilRelease);   // acquire and hold the GIL until done...
        auto curThread = threadHead(head);
        return GetPythonThreadId(version, curThread);
    }
    
    
    /*
     * Returns nullptr or a PyObject* list with the thread ids. 
     */
    DECLDIR void* list_all_thread_ids() {
        ModuleInfo moduleInfo = GetPythonModule();
        if (moduleInfo.errorGettingModule != 0) {
            PRINT("Error getting python module");
            
            // We can't even use something as Py_BuildValue("") here, so, the caller must explicitly check for null or PyObject*.
            // (thus, we return a void*).
            return nullptr; 
        }
        HMODULE module = moduleInfo.module;
        auto version = GetPythonVersion(module);
        
        DEFINE_PROC(gilEnsure, PyGILState_Ensure*, "PyGILState_Ensure", nullptr);
        DEFINE_PROC(gilRelease, PyGILState_Release*, "PyGILState_Release", nullptr);
        DEFINE_PROC(pylistNew, PyList_New*, "PyList_New", nullptr);
        DEFINE_PROC(pylistAppend, PyList_Append*, "PyList_Append", nullptr);
        DEFINE_PROC(interpHead, PyInterpreterState_Head*, "PyInterpreterState_Head", nullptr);
        DEFINE_PROC(threadHead, PyInterpreterState_ThreadHead*, "PyInterpreterState_ThreadHead", nullptr);
        DEFINE_PROC(threadNext, PyThreadState_Next*, "PyThreadState_Next", nullptr);
        
        PyInt_FromLong* intFromLong;
        
        if (version >= PythonVersion_30) {
            DEFINE_PROC(intFromLongPy3, PyInt_FromLong*, "PyLong_FromLong", nullptr);
            intFromLong = intFromLongPy3;
        } else {
            DEFINE_PROC(intFromLongPy2, PyInt_FromLong*, "PyInt_FromLong", nullptr);
            intFromLong = intFromLongPy2;
        }
        
        auto head = interpHead();
        if (head == nullptr) {
            // this interpreter is loaded but not initialized.
            PRINT("Interpreter not initialized!");
            return nullptr;
        }

        GilHolder gilLock(gilEnsure, gilRelease);   // acquire and hold the GIL until done...

        PyObject* lst = pylistNew(0);
        
        auto curThread = threadHead(head);
        if (curThread == nullptr) {
            PRINT("Thread head is NULL.")
            return nullptr;
        }
        
        for (auto curThread = threadHead(head); curThread != nullptr; curThread = threadNext(curThread)) {
            DWORD pythonThreadId = GetPythonThreadId(version, curThread);
            PyObject *asPyInt = intFromLong(pythonThreadId);
            pylistAppend(lst, asPyInt);
            DecRef(asPyInt, moduleInfo.isDebug); // Only the list is holding the reference now.
        }

        return lst;
    }
    
    DECLDIR int PrintDebugInfo() {
        PRINT("Getting debug info...");
        ModuleInfo moduleInfo = GetPythonModule();
        if (moduleInfo.errorGettingModule != 0) {
            PRINT("Error getting python module");
            return 0;
        }
        HMODULE module = moduleInfo.module;
        
        DEFINE_PROC(interpHead, PyInterpreterState_Head*, "PyInterpreterState_Head", 0);
        DEFINE_PROC(threadHead, PyInterpreterState_ThreadHead*, "PyInterpreterState_ThreadHead", 0);
        DEFINE_PROC(threadNext, PyThreadState_Next*, "PyThreadState_Next", 160);
        DEFINE_PROC(gilEnsure, PyGILState_Ensure*, "PyGILState_Ensure", 0);
        DEFINE_PROC(gilRelease, PyGILState_Release*, "PyGILState_Release", 0);
        
        auto head = interpHead();
        if (head == nullptr) {
            // this interpreter is loaded but not initialized.
            PRINT("Interpreter not initialized!");
            return 0;
        }
        
        auto version = GetPythonVersion(module);
        printf("Python version: %d\n", version);

        GilHolder gilLock(gilEnsure, gilRelease);   // acquire and hold the GIL until done...
        auto curThread = threadHead(head);
        if (curThread == nullptr) {
            PRINT("Thread head is NULL.")
            return 0;
        }
        
        for (auto curThread = threadHead(head); curThread != nullptr; curThread = threadNext(curThread)) {
            printf("Found thread id: %d\n", GetPythonThreadId(version, curThread));
        }

        PRINT("Finished getting debug info.")
        return 0;
    }
    
    
    /**
     * This function may be called to set a tracing function to existing python threads.
     **/
    DECLDIR int AttachDebuggerTracing(bool showDebugInfo, void* pSetTraceFunc, void* pTraceFunc, unsigned int threadId)
    {
        ModuleInfo moduleInfo = GetPythonModule();
        if (moduleInfo.errorGettingModule != 0) {
            return moduleInfo.errorGettingModule;
        }
        HMODULE module = moduleInfo.module;
        if (showDebugInfo) {
            std::cout << "Setting sys trace for existing threads." << std::endl << std::flush;
        }
        int attached = 0;
        PyObjectHolder traceFunc(moduleInfo.isDebug, (PyObject*) pTraceFunc, true);
        PyObjectHolder setTraceFunc(moduleInfo.isDebug, (PyObject*) pSetTraceFunc, true);
        int temp = InternalSetSysTraceFunc(module, moduleInfo.isDebug, showDebugInfo, &traceFunc, &setTraceFunc, threadId);
        if (temp == 0) {
            // we've successfully attached the debugger
            return 0;
        } else {
           if (temp > attached) {
               //I.e.: the higher the value the more significant it is.
               attached = temp;
            }
        }

        if (showDebugInfo) {
            std::cout << "Setting sys trace for existing threads failed with code: " << attached << "." << std::endl << std::flush;
        }
        return attached;
    }

}