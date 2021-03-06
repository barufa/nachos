/// Entry points into the Nachos kernel from user programs.
///
/// There are two kinds of things that can cause control to transfer back to
/// here from user code:
///
/// * System calls: the user code explicitly requests to call a procedure in
///   the Nachos kernel.  Right now, the only function we support is `Halt`.
///
/// * Exceptions: the user code does something that the CPU cannot handle.
///   For instance, accessing memory that does not exist, arithmetic errors,
///   etc.
///
/// Interrupts (which can also cause control to transfer from user code into
/// the Nachos kernel) are handled elsewhere.
///
/// For now, this only handles the `Halt` system call.  Everything else core-
/// dumps.
///
/// Copyright (c) 1992-1993 The Regents of the University of California.
///               2016-2019 Docentes de la Universidad Nacional de Rosario.
/// All rights reserved.  See `copyright.h` for copyright notice and
/// limitation of liability and disclaimer of warranty provisions.


#include "transfer.hh"
#include "syscall.h"
#include "filesys/directory_entry.hh"
#include "threads/system.hh"
#include "args.hh"
#include <string.h>

void
Copy(const char * unixFile, const char * nachosFile);

static void
IncrementPC()
{
    unsigned pc;

    pc = machine->ReadRegister(PC_REG);
    machine->WriteRegister(PREV_PC_REG, pc);
    pc = machine->ReadRegister(NEXT_PC_REG);
    machine->WriteRegister(PC_REG, pc);
    pc += 4;
    machine->WriteRegister(NEXT_PC_REG, pc);
}

/// Do some default behavior for an unexpected exception.
///
/// * `et` is the kind of exception.  The list of possible exceptions is in
///   `machine/exception_type.hh`.
static void
DefaultHandler(ExceptionType et)
{
    int exceptionArg = machine->ReadRegister(2);

    fprintf(stderr, "Unexpected user mode exception: %s, arg %d.\n",
      ExceptionTypeToString(et), exceptionArg);
    ASSERT(false);
}

/// Handle a system call exception.
///
/// * `et` is the kind of exception.  The list of possible exceptions is in
///   `machine/exception_type.hh`.
///
/// The calling convention is the following:
///
/// * system call identifier in `r2`;
/// * 1st argument in `r4`;
/// * 2nd argument in `r5`;
/// * 3rd argument in `r6`;
/// * 4th argument in `r7`;
/// * the result of the system call, if any, must be put back into `r2`.
///
/// And do not forget to increment the program counter before returning. (Or
/// else you will loop making the same system call forever!)

void
machine_ret(int r)
{
    machine->WriteRegister(2, r);
}

void
run_program(void * arg)
{
    currentThread->space->InitRegisters();
    currentThread->space->RestoreState();

    int * args = WriteArgs((char **) arg);
    int argc   = args[0];
    int argv   = args[1];

    DEBUG('g', "argc = %d - argv = %d in run_program\n", argc, argv);

    machine->WriteRegister(4, argc);
    machine->WriteRegister(5, argv);

    machine->Run();
}

static void
SyscallHandler(ExceptionType _et)
{
    int scid = machine->ReadRegister(2);// r2
    int arg1 = machine->ReadRegister(4);// r4
    int arg2 = machine->ReadRegister(5);// r5
    int arg3 = machine->ReadRegister(6);// r6

    switch (scid) {
        case SC_HALT: {// Codeado
            DEBUG('e', "Calling SC_HALT.\n");
            DEBUG('e', "Shutdown, initiated by user program.\n");
            interrupt->Halt();
            break;
        }
        case SC_CREATE: {// Codeado
            DEBUG('e', "Calling SC_CREATE\n");
            int filenameAddr = arg1;
            if (filenameAddr == 0)
                DEBUG('e', "Error: address to filename string is null.\n");

            char filename[FILE_NAME_MAX_LEN + 1];
            if (!ReadStringFromUser(filenameAddr, filename, sizeof filename)) {
                DEBUG('e',
                  "Error: filename string too long (maximum is %u bytes).\n",
                  FILE_NAME_MAX_LEN);
            }
            DEBUG('e', "Open requested for file `%s`.\n", filename);
            machine_ret(fileSystem->Create(filename));
            break;
        }
        case SC_WRITE: {// Codedado
            DEBUG('e', "Calling SC_WRITE.\n");
            int buffer    = arg1;
            int size      = arg2;
            OpenFileId id = arg3;
            int r         = -1;

            if (size <= 0) break;
            ASSERT(buffer);

            switch (id) {
                case CONSOLE_OUTPUT: {// STDOUT
                    char * bff = new char[size + 1];
                    ReadBufferFromUser(buffer, bff, size);
                    r = synchConsole->PutString(bff, size);
                    delete[] bff;
                    break;
                }
                default: {
                    if (currentThread->IsOpenFile(id)) {
                        OpenFile * file = currentThread->GetFile(id);
                        char * bff      = new char[size];
                        ReadBufferFromUser(buffer, bff, size);
                        r = file->Write(bff, size);
                        delete[] bff;
                    }
                    break;
                }
            }
            machine_ret(r);
            break;
        }
        case SC_OPEN: {// Codeado
            DEBUG('e', "Calling SC_OPEN.\n");
            int nameaddr = arg1;
            int r        = -1;

            char * filename = new char[FILE_NAME_MAX_LEN + 1];
            if (ReadStringFromUser(nameaddr, filename, FILE_NAME_MAX_LEN)) {
                OpenFile * file = fileSystem->Open(filename);
                r = currentThread->AddFile(file);
            }

            delete[] filename;
            machine_ret(r);
            break;
        }
        case SC_CLOSE: {// Codeado
            DEBUG('e', "Calling SC_CLOSE.\n");
            int fid = machine->ReadRegister(4);
            int r   = -1;
            DEBUG('e', "Close requested for id %u.\n", fid);
            if (currentThread->IsOpenFile(fid)) {
                OpenFile * file = currentThread->RemoveFile(fid);
                delete file;
            }
            machine_ret(r);
            break;
        }
        case SC_EXIT: {// Codeado
            DEBUG('e', "Calling SC_EXIT.\n");
            machine_ret(arg1);
            currentThread->Finish(arg1);
            break;
        }
        case SC_JOIN: {// Codeado
            DEBUG('e', "Calling SC_JOIN.\n");
            SpaceId id = arg1;
            if (!(processTable->HasKey(id))) {
                DEBUG('e', "Invalid pid %d.\n", id);
                break;
            }
            DEBUG('e', "The userland/program is joining\n");
            int r = (processTable->Get(id))->Join();
            machine_ret(r);
            break;
        }
        case SC_EXEC: {// Codeado
            DEBUG('e', "Calling SC_EXEC.\n");
            int nameaddr  = arg1;
            int argv      = arg2;
            int join_flag = arg3;
            int r         = -1;
            void * argvs  = (void *) SaveArgs(argv);

            char * filename = new char[FILE_NAME_MAX_LEN + 1];

            if (ReadStringFromUser(nameaddr, filename, FILE_NAME_MAX_LEN)) {
                DEBUG('e', "Opening %s file to execute\n", filename);
                DEBUG('e', "The program is executing with join_flag=%d\n",
                  join_flag);
                OpenFile * executable = fileSystem->Open(filename);
                Thread * newThread    = new Thread("Child_Thread", join_flag);
                newThread->space = new AddressSpace(executable);
                r = newThread->pid;
                newThread->Fork(run_program, (void *) argvs);
            }

            delete[] filename;
            machine_ret(r);
            break;
        }
        case SC_READ: {// Codeado
            DEBUG('e', "Calling SC_READ.\n");
            int buffer    = arg1;
            int size      = arg2;
            OpenFileId id = arg3;
            int r         = -1;

            ASSERT(buffer);
            ASSERT(0 < size);

            switch (id) {
                case CONSOLE_INPUT: {
                    char * bff = new char[size + 1];
                    r = synchConsole->GetString(bff, size);
                    WriteBufferToUser(buffer, bff, r);
                    DEBUG('e', "Read: %s[%d]\n", bff, r);
                    delete[] bff;
                    break;
                }
                default: {
                    if (currentThread->IsOpenFile(id)) {
                        OpenFile * file = currentThread->GetFile(id);
                        char * bff      = new char[size];
                        memset(bff, 0, size);
                        r = file->Read(bff, size);
                        WriteBufferToUser(buffer, bff, r);
                        DEBUG('e', "Read: %s", bff);
                        delete[] bff;
                    }
                    break;
                }
            }
            machine_ret(r);
            break;
        }
        case SC_REMOVE: {// Codeado
            DEBUG('e', "Calling SC_REMOVE\n");
            int filenameAddr = arg1;
            if (filenameAddr == 0)
                DEBUG('e', "Error: address to filename string is null.\n");

            char filename[FILE_NAME_MAX_LEN + 1];
            if (!ReadStringFromUser(filenameAddr, filename, sizeof filename)) {
                DEBUG('e',
                  "Error: filename string too long (maximum is %u bytes).\n",
                  FILE_NAME_MAX_LEN);
            }
            machine_ret(fileSystem->Remove(filename));
            break;
        }
        default: {
            fprintf(stderr, "Unexpected system call: id %d.\n", scid);
            ASSERT(false);
        }
    }

    IncrementPC();
} // SyscallHandler

static void
Page_Fault_Handler(ExceptionType _et)
{
    // buscar en la pageTable, y insertar en la TBL
    unsigned vpn = machine->ReadRegister(BAD_VADDR_REG) / PAGE_SIZE;

    if (!currentThread->space->Update_TLB(vpn)) {
        currentThread->Finish(-1);
    }
    DEBUG('a', "Saliendo de Page_Fault_Handler\n");
}

static void
Read_Only_Handler(ExceptionType _et)
{
    DEBUG('a', "Read only exception\n");
    currentThread->Finish();
}

/// By default, only system calls have their own handler.  All other
/// exception types are assigned the default handler.
void
SetExceptionHandlers()
{
    machine->SetHandler(NO_EXCEPTION, &DefaultHandler);
    machine->SetHandler(SYSCALL_EXCEPTION, &SyscallHandler);
    machine->SetHandler(PAGE_FAULT_EXCEPTION, &Page_Fault_Handler);
    machine->SetHandler(READ_ONLY_EXCEPTION, &Read_Only_Handler);
    machine->SetHandler(BUS_ERROR_EXCEPTION, &DefaultHandler);
    machine->SetHandler(ADDRESS_ERROR_EXCEPTION, &DefaultHandler);
    machine->SetHandler(OVERFLOW_EXCEPTION, &DefaultHandler);
    machine->SetHandler(ILLEGAL_INSTR_EXCEPTION, &DefaultHandler);
}
