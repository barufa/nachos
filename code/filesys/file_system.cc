/// Routines to manage the overall operation of the file system.  Implements
/// routines to map from textual file names to files.
///
/// Each file in the file system has:
/// * a file header, stored in a sector on disk (the size of the file header
///   data structure is arranged to be precisely the size of 1 disk sector);
/// * a number of data blocks;
/// * an entry in the file system directory.
///
/// The file system consists of several data structures:
/// * A bitmap of free disk sectors (cf. `bitmap.h`).
/// * A directory of file names and file headers.
///
/// Both the bitmap and the directory are represented as normal files.  Their
/// file headers are located in specific sectors (sector 0 and sector 1), so
/// that the file system can find them on bootup.
///
/// The file system assumes that the bitmap and directory files are kept
/// “open” continuously while Nachos is running.
///
/// For those operations (such as `Create`, `Remove`) that modify the
/// directory and/or bitmap, if the operation succeeds, the changes are
/// written immediately back to disk (the two files are kept open during all
/// this time).  If the operation fails, and we have modified part of the
/// directory and/or bitmap, we simply discard the changed version, without
/// writing it back to disk.
///
/// Our implementation at this point has the following restrictions:
///
/// * there is no synchronization for concurrent accesses;
/// * files have a fixed size, set when the file is created;
/// * files cannot be bigger than about 3KB in size;
/// * there is no hierarchical directory structure, and only a limited number
///   of files can be added to the system;
/// * there is no attempt to make the system robust to failures (if Nachos
///   exits in the middle of an operation that modifies the file system, it
///   may corrupt the disk).
///
/// Copyright (c) 1992-1993 The Regents of the University of California.
///               2016-2018 Docentes de la Universidad Nacional de Rosario.
/// All rights reserved.  See `copyright.h` for copyright notice and
/// limitation of liability and disclaimer of warranty provisions.

#include "file_system.hh"
#include "directory.hh"
#include "directory_entry.hh"
#include "file_header.hh"
#include "threads/system.hh"
#include "machine/disk.hh"
#include "lib/bitmap.hh"

#include "string.h"
/// Sectors containing the file headers for the bitmap of free sectors, and
/// the directory of files.  These file headers are placed in well-known
/// sectors, so that they can be located on boot-up.
static const unsigned FREE_MAP_SECTOR  = 0;
static const unsigned DIRECTORY_SECTOR = 1;

/// Initial file sizes for the bitmap and directory; until the file system
/// supports extensible files, the directory size sets the maximum number of
/// files that can be loaded onto the disk.
static const unsigned FREE_MAP_FILE_SIZE  = NUM_SECTORS / BITS_IN_BYTE;
static const unsigned DIRECTORY_FILE_SIZE = sizeof(DirectoryEntry)
  * NUM_DIR_ENTRIES;

static const char *
getName(const char * name)
{
    int pos = strlen(name) - 1;

    // No deberia pasar, pero...
    if (name[pos] == '/') {
        pos--;
    } else if (name[0] != '/') {
        return name;
    }

    for (unsigned i = pos; 0 <= i; i--) {
        if (name[i] == '/') {
            return name + i + 1;
        }
    }
    return name;
}

static const char *
getParent(const char * path)
{
    char * parent = new char[PATH_MAX_LEN];

    strncpy(parent, path, PATH_MAX_LEN);

    DEBUG('F', "Buscando padre de %s\n", path);

    int pos = strlen(parent) - 1;
    if (parent[pos] == '/') {
        pos--;
    }

    for (unsigned i = pos; i >= 0; i--) {
        if (parent[i] == '/') {
            parent[i + 1] = '\0';
            return parent;
        }
    }
    ASSERT(false);
    return parent;
}

static const char *
CheckRoot(const char * _path)
{
    ASSERT(_path != nullptr);

    if (_path[0] != '/') {
        char * path  = new char[2 * FILE_NAME_MAX_LEN];
        unsigned len = strlen(currentThread->GetPath());
        strncpy(path, currentThread->GetPath(), len);
        if (path[len - 1] != '/') {
            path[len] = '/';
            len++;
        }
        strncpy(path + len, _path, strlen(_path));
        return path;
    } else {
        return _path;
    }

    return nullptr;
}

Directory *
FileSystem::OpenPath(const char * __path, int * _sector)
{
    ASSERT(__path != nullptr);
    ASSERT(_sector != nullptr);

    const char * _path = CheckRoot(__path);
    DEBUG('F', "_path: \"%s\"", _path);
    OpenFile * dir_file = nullptr;
    int sector = DIRECTORY_SECTOR;
    Directory * dir = new Directory(NUM_DIR_ENTRIES);
    char * path = new char[PATH_MAX_LEN], * p;
    strncpy(path, _path + 1, strlen(_path) - 1);
    p = path;
    DEBUG('F', "Abriendo %s\n", path);

    dir->FetchFrom(directoryFile);
    unsigned l = strlen(path);
    for (unsigned i = 0; i < l; i++) {
        if (path[i] == '/') {
            path[i] = '\0';
            sector  = dir->Find(p, true);
            if (sector == -1) {
                DEBUG('F', "No existe %s en %s\n", p, _path);
                return nullptr;
            } else {
                DEBUG('F', "Accediendo a directorio %s\n", p);
                dir_file = new OpenFile(sector);
                dir->FetchFrom(dir_file);
                delete dir_file;
            }
            p += i + 1;
        }
    }
    *_sector = sector;

    delete[] path;
    return dir;
} // FileSystem::OpenPath

/// Initialize the file system.  If `format == true`, the disk has nothing on
/// it, and we need to initialize the disk to contain an empty directory, and
/// a bitmap of free sectors (with almost but not all of the sectors marked
/// as free).
///
/// If `format == false`, we just have to open the files representing the
/// bitmap and the directory.
///
/// * `format` -- should we initialize the disk?
FileSystem::FileSystem(bool format)
{
    DEBUG('F', "Initializing the file system.\n");
    if (format) {
        Bitmap * freeMap       = new Bitmap(NUM_SECTORS);
        Directory * directory  = new Directory(NUM_DIR_ENTRIES);
        FileHeader * mapHeader = new FileHeader;
        FileHeader * dirHeader = new FileHeader;

        DEBUG('F', "Formatting the file system.\n");

        // First, allocate space for FileHeaders for the directory and bitmap
        // (make sure no one else grabs these!)
        freeMap->Mark(FREE_MAP_SECTOR);
        freeMap->Mark(DIRECTORY_SECTOR);

        // Second, allocate space for the data blocks containing the contents
        // of the directory and bitmap files.  There better be enough space!

        ASSERT(mapHeader->Allocate(freeMap, FREE_MAP_FILE_SIZE));
        ASSERT(dirHeader->Allocate(freeMap, DIRECTORY_FILE_SIZE));

        // Flush the bitmap and directory `FileHeader`s back to disk.
        // We need to do this before we can `Open` the file, since open reads
        // the file header off of disk (and currently the disk has garbage on
        // it!).

        DEBUG('F', "Writing headers back to disk.\n");
        mapHeader->WriteBack(FREE_MAP_SECTOR);
        dirHeader->WriteBack(DIRECTORY_SECTOR);

        // OK to open the bitmap and directory files now.
        // The file system operations assume these two files are left open
        // while Nachos is running.

        freeMapFile   = new OpenFile(FREE_MAP_SECTOR);
        directoryFile = new OpenFile(DIRECTORY_SECTOR);

        // Once we have the files “open”, we can write the initial version of
        // each file back to disk.  The directory at this point is completely
        // empty; but the bitmap has been changed to reflect the fact that
        // sectors on the disk have been allocated for the file headers and
        // to hold the file data for the directory and bitmap.

        DEBUG('F', "Writing bitmap and directory back to disk.\n");
        freeMap->WriteBack(freeMapFile); // flush changes to disk
        directory->WriteBack(directoryFile);

        if (debug.IsEnabled('f')) {
            freeMap->Print();
            directory->Print();

            delete freeMap;
            delete directory;
            delete mapHeader;
            delete dirHeader;
        }
    } else {
        // If we are not formatting the disk, just open the files
        // representing the bitmap and directory; these are left open while
        // Nachos is running.
        freeMapFile   = new OpenFile(FREE_MAP_SECTOR);
        directoryFile = new OpenFile(DIRECTORY_SECTOR);
    }
}

FileSystem::~FileSystem()
{
    delete freeMapFile;
    delete directoryFile;
}

/// Create a file in the Nachos file system (similar to UNIX `create`).
/// Since we cannot increase the size of files dynamically, we have to give
/// Create the initial size of the file.
///
/// The steps to create a file are:
/// 1. Make sure the file does not already exist.
/// 2. Allocate a sector for the file header.
/// 3. Allocate space on disk for the data blocks for the file.
/// 4. Add the name to the directory.
/// 5. Store the new file header on disk.
/// 6. Flush the changes to the bitmap and the directory back to disk.
///
/// Return true if everything goes ok, otherwise, return false.
///
/// Create fails if:
/// * file is already in directory;
/// * no free space for file header;
/// * no free entry for file in directory;
/// * no free space for data blocks for the file.
///
/// Note that this implementation assumes there is no concurrent access to
/// the file system!
///
/// * `name` is the name of file to be created.
/// * `initialSize` is the size of file to be created.
bool
FileSystem::Create(const char * _path, unsigned initialSize)
{
    ASSERT(_path != nullptr);

    const char * path = CheckRoot(_path);

    int sector, dir_sector = DIRECTORY_SECTOR;
    Directory * directory = OpenPath(path, &dir_sector);
    Bitmap * freeMap;
    FileHeader * header;
    bool success;
    const char * name = getName(path);

    DEBUG('F', "Creating file %s, size %u\n", name, initialSize);

    if (directory == nullptr ||
      directory->Find(name, true) != -1 ||
      directory->Find(name, false) != -1)
    {
        DEBUG('F', "No encuentra el directorio o el nombre ya existe\n");
        success = false; // File is already in directory.
    } else {
        freeMap = new Bitmap(NUM_SECTORS);
        freeMap->FetchFrom(freeMapFile);
        sector = freeMap->Find(); // Find a sector to hold the file header.
        if (sector == -1) {
            success = false; // No free block for file header.
        } else if (!directory->Add(name, sector)) {
            success = false; // No space in directory.
        } else {
            header = new FileHeader;
            if (!header->Allocate(freeMap, initialSize)) {
                success = false; // No space on disk for data.
            } else {
                success = true;
                // Everthing worked, flush all changes back to disk.
                header->WriteBack(sector);
                freeMap->WriteBack(freeMapFile);
                if (dir_sector == DIRECTORY_SECTOR) {
                    directory->WriteBack(directoryFile);
                } else {
                    OpenFile * f = new OpenFile(dir_sector);
                    directory->WriteBack(f);
                    delete f;
                }
            }
            delete header;
        }
        delete freeMap;
    }
    delete directory;
    if (success) {
        DEBUG('F', "Archivo %s creado\n", path);
    }

    return success;
} // FileSystem::Create

/// Open a file for reading and writing.
///
/// To open a file:
/// 1. Find the location of the file's header, using the directory.
/// 2. Bring the header into memory.
///
/// * `name` is the text name of the file to be opened.
OpenFile *
FileSystem::Open(const char * _path)
{
    ASSERT(_path != nullptr);

    const char * path = CheckRoot(_path);

    int sector, dir_sector;
    OpenFile * openFile   = nullptr;
    Directory * directory = OpenPath(path, &dir_sector);
    const char * name     = getName(path);

    DEBUG('F', "Opening file %s en %s\n", name, path);
    sector = directory->Find(name);
    if (sector > 1) {// `name` was found in directory.
        Filenode * node = filetable->find(sector);
        if (node == nullptr)
            node = filetable->add_file(name, sector);
        if (node->remove == false) {
            node->users++;
            openFile = new OpenFile(sector);
        }
    }
    delete directory;
    return openFile; // Return null if not found.
}

/// Delete a file from the file system.
///
/// This requires:
/// 1. Remove it from the directory.
/// 2. Delete the space for its header.
/// 3. Delete the space for its data blocks.
/// 4. Write changes to directory, bitmap back to disk.
///
/// Return true if the file was deleted, false if the file was not in the
/// file system.
///
/// * `name` is the text name of the file to be removed.
bool
FileSystem::Remove(const char * _path)
{
    ASSERT(_path != nullptr);

    const char * path = CheckRoot(_path);

    int sector, dir_sector;
    Directory * directory = OpenPath(path, &dir_sector);
    const char * name     = getName(path);

    sector = directory->Find(name);
    if (sector < 0) {
        sector = directory->Find(name, true);
        if (sector < 0) {
            delete directory;
            return false; // file not found
        }
        return RemoveDir(path);
    }
    // Si hay alguien usando el archivo, solo lo marco para eliminar
    Filenode * node = filetable->find(sector);
    if (node != nullptr && node->users != 0) {
        node->remove = true;
    } else {
        Bitmap * freeMap;
        FileHeader * fileHeader = new FileHeader;

        directory->Remove(name);
        freeMap = new Bitmap(NUM_SECTORS);
        freeMap->FetchFrom(freeMapFile);

        // Aca tira la falla
        fileHeader->FetchFrom(sector);
        fileHeader->Deallocate(freeMap); // Remove data blocks.
        freeMap->Clear(sector);          // Remove header block.

        freeMap->WriteBack(freeMapFile); // Flush to disk.

        if (dir_sector == DIRECTORY_SECTOR) {
            directory->WriteBack(directoryFile); // Flush to disk.
        } else {
            OpenFile * f = new OpenFile(dir_sector);
            directory->WriteBack(f);
            delete f;
        }

        filetable->remove(sector);

        delete fileHeader;
        delete freeMap;
    }
    delete directory;

    DEBUG('F', "Se elimino el archivo\n");

    return true;
} // FileSystem::Remove

/// List all the files in the file system directory.
void
FileSystem::List(const char * path)
{
    Directory * directory = nullptr;

    if (path == nullptr) {
        return List(currentThread->GetPath());
    } else if (strcmp(path, "/") == 0) {
        directory = new Directory(NUM_DIR_ENTRIES);
        directory->FetchFrom(directoryFile);
    } else {
        int _sec = 0;
        const char * _path = CheckRoot(path);
        directory = OpenPath(_path, &_sec);
    }

    if (directory != nullptr) {
        directory->Get_List();
        delete directory;
    }
}

static bool
AddToShadowBitmap(unsigned sector, Bitmap * map)
{
    ASSERT(map != nullptr);

    if (map->Test(sector)) {
        DEBUG('F', "Sector %u was already marked.\n", sector);
        return false;
    }
    map->Mark(sector);
    DEBUG('F', "Marked sector %u.\n", sector);
    return true;
}

static bool
CheckForError(bool value, const char * message)
{
    if (!value)
        DEBUG('F', message);
    return !value;
}

static bool
CheckSector(unsigned sector, Bitmap * shadowMap)
{
    bool error = false;

    error |= CheckForError(sector < NUM_SECTORS, "Sector number too big.\n");
    error |= CheckForError(AddToShadowBitmap(sector, shadowMap),
        "Sector number already used.\n");
    return error;
}

static bool
CheckFileHeader(const RawFileHeader * rh, unsigned num, Bitmap * shadowMap)
{
    ASSERT(rh != nullptr);

    bool error = false;

    DEBUG('F',
      "Checking file header %u.  File size: %u bytes, number of sectors: %u.\n",
      num, rh->numBytes, rh->numSectors);
    error |= CheckForError(rh->numSectors >= DivRoundUp(rh->numBytes,
        SECTOR_SIZE),
        "Sector count not compatible with file size.\n");
    error |= CheckForError(rh->numSectors < NUM_DIRECT,
        "Too many blocks.\n");
    for (unsigned i = 0; i < rh->numSectors; i++) {
        unsigned s = rh->dataSectors[i];
        error |= CheckSector(s, shadowMap);
    }
    return error;
}

static bool
CheckBitmaps(const Bitmap * freeMap, const Bitmap * shadowMap)
{
    bool error = false;

    for (unsigned i = 0; i < NUM_SECTORS; i++) {
        DEBUG('F', "Checking sector %u. Original: %u, shadow: %u.\n",
          i, freeMap->Test(i), shadowMap->Test(i));
        error |= CheckForError(freeMap->Test(i) == shadowMap->Test(i),
            "Inconsistent bitmap.");
    }
    return error;
}

static bool
CheckDirectory(const RawDirectory * rd, Bitmap * shadowMap)
{
    ASSERT(rd != nullptr);
    ASSERT(shadowMap != nullptr);

    bool error         = false;
    unsigned nameCount = 0;
    const char * knownNames[NUM_DIR_ENTRIES];

    for (unsigned i = 0; i < NUM_DIR_ENTRIES; i++) {
        DEBUG('F', "Checking direntry: %u.\n", i);
        const DirectoryEntry * e = &rd->table[i];

        if (e->inUse) {
            if (strlen(e->name) > FILE_NAME_MAX_LEN) {
                DEBUG('F', "Filename too long.\n");
                error = true;
            }

            // Check for repeated filenames.
            DEBUG('F', "Checking for repeated names.  Name count: %u.\n",
              nameCount);
            bool repeated = false;
            for (unsigned j = 0; j < nameCount; j++) {
                DEBUG('F', "Comparing \"%s\" and \"%s\".\n",
                  knownNames[j], e->name);
                if (strcmp(knownNames[j], e->name) == 0) {
                    DEBUG('F', "Repeated filename.\n");
                    repeated = true;
                    error    = true;
                }
            }
            if (!repeated) {
                knownNames[nameCount] = e->name;
                DEBUG('F', "Added \"%s\" at %u.\n", e->name, nameCount);
                nameCount++;
            }

            // Check sector.
            error |= CheckSector(e->sector, shadowMap);

            // Check file header.
            FileHeader * h = new FileHeader;
            const RawFileHeader * rh = h->GetRaw();
            h->FetchFrom(e->sector);
            error |= CheckFileHeader(rh, e->sector, shadowMap);
            delete h;
        }
    }
    return error;
} // CheckDirectory

bool
FileSystem::Check() //Obsolete
{
    DEBUG('F', "This filesystem check is out of date\n");
    return false;
    // DEBUG('F', "Performing filesystem check\n");
    // bool error = false;
    //
    // Bitmap * shadowMap = new Bitmap(NUM_SECTORS);
    // shadowMap->Mark(FREE_MAP_SECTOR);
    // shadowMap->Mark(DIRECTORY_SECTOR);
    //
    // DEBUG('F', "Checking bitmap's file header.\n");
    //
    // FileHeader * bitH = new FileHeader;
    // const RawFileHeader * bitRH = bitH->GetRaw();
    // bitH->FetchFrom(FREE_MAP_SECTOR);
    // DEBUG('F', "  File size: %u bytes, expected %u bytes.\n"
    //   "  Number of sectors: %u, expected %u.\n",
    //   bitRH->numBytes, FREE_MAP_FILE_SIZE,
    //   bitRH->numSectors, FREE_MAP_FILE_SIZE / SECTOR_SIZE);
    // error |= CheckForError(bitRH->numBytes == FREE_MAP_FILE_SIZE,
    //     "Bad bitmap header: wrong file size.\n");
    // error |= CheckForError(
    //     bitRH->numSectors == FREE_MAP_FILE_SIZE / SECTOR_SIZE,
    //     "Bad bitmap header: wrong number of sectors.\n");
    // error |= CheckFileHeader(bitRH, FREE_MAP_SECTOR, shadowMap);
    // delete bitH;
    //
    // DEBUG('F', "Checking directory.\n");
    //
    // FileHeader * dirH = new FileHeader;
    // const RawFileHeader * dirRH = dirH->GetRaw();
    // dirH->FetchFrom(DIRECTORY_SECTOR);
    // error |= CheckFileHeader(dirRH, DIRECTORY_SECTOR, shadowMap);
    // delete dirH;
    //
    // Bitmap * freeMap = new Bitmap(NUM_SECTORS);
    // freeMap->FetchFrom(freeMapFile);
    // Directory * dir = new Directory(NUM_DIR_ENTRIES);
    // const RawDirectory * rdir = dir->GetRaw();
    // dir->FetchFrom(directoryFile);
    // error |= CheckDirectory(rdir, shadowMap);
    // delete dir;
    //
    // // The two bitmaps should match.
    // DEBUG('F', "Checking bitmap consistency.\n");
    // error |= CheckBitmaps(freeMap, shadowMap);
    // delete shadowMap;
    // delete freeMap;
    //
    // DEBUG('F', error ? "Filesystem check succeeded.\n" :
    //   "Filesystem check failed.\n");
    //
    // return !error;
} // FileSystem::Check

/// Print everything about the file system:
/// * the contents of the bitmap;
/// * the contents of the directory;
/// * for each file in the directory:
///   * the contents of the file header;
///   * the data in the file.
void
FileSystem::Print()
{
    FileHeader * bitHeader = new FileHeader;
    FileHeader * dirHeader = new FileHeader;
    Bitmap * freeMap       = new Bitmap(NUM_SECTORS);
    Directory * directory  = new Directory(NUM_DIR_ENTRIES);

    printf("--------------------------------\n"
      "Bit map file header:\n\n");
    bitHeader->FetchFrom(FREE_MAP_SECTOR);
    bitHeader->Print();

    printf("--------------------------------\n"
      "Directory file header:\n\n");
    dirHeader->FetchFrom(DIRECTORY_SECTOR);
    dirHeader->Print();

    printf("--------------------------------\n");
    freeMap->FetchFrom(freeMapFile);
    freeMap->Print();

    printf("--------------------------------\n");
    directory->FetchFrom(directoryFile);
    directory->Print();
    printf("--------------------------------\n");

    delete bitHeader;
    delete dirHeader;
    delete freeMap;
    delete directory;
}

bool
FileSystem::Expand(unsigned sector, unsigned size)
{
    FileHeader * header = new FileHeader;
    Bitmap * freeMap    = new Bitmap(NUM_SECTORS);
    bool ret = false;

    header->FetchFrom(sector);
    freeMap->FetchFrom(freeMapFile);
    if (header->Extend(freeMap, size)) {
        freeMap->WriteBack(freeMapFile);
        header->WriteBack(sector);
        ret = true;
    }
    delete freeMap;
    delete header;
    return ret;
}

bool
FileSystem::MakeDir(const char * _path)
{
    ASSERT(_path != nullptr);

    const char * path = CheckRoot(_path);

    int dir_sector, sector;
    FileHeader * header;
    Bitmap * freeMap;
    const char * parent_path = getParent(path);
    const char * name        = getName(path);
    Directory * directory    = OpenPath(parent_path, &dir_sector);
    bool success = true;

    DEBUG('F', "Creando el directorio %s en %s\n", name, parent_path);

    if (directory == nullptr ||
      directory->Find(name, true) != -1 ||
      directory->Find(name, false) != -1)
    {
        return false;
    }

    directory->Get_List();

    if (directory->Find(name,
      true) != -1 || directory->Find(name, false) != -1)
    {
        DEBUG('F', "El directorio %s ya existe\n", name);
        delete directory;
        return false;
    }

    freeMap = new Bitmap(NUM_SECTORS);
    freeMap->FetchFrom(freeMapFile);
    sector = freeMap->Find(); // Find a sector to hold the file header.
    if (sector == -1) {
        DEBUG('F', "No hay sufuciente espacio en el disco\n");
        success = false; // No free block for file header.
    } else if (!directory->Add(name, sector, true)) {
        success = false; // No space in directory.
    } else {
        DEBUG('F', "Alocando espacio para el directorio\n");
        header = new FileHeader;
        if (!header->Allocate(freeMap, DIRECTORY_FILE_SIZE)) {
            success = false; // No space on disk for data.
            directory->Remove(name);
            if (freeMap->Test(sector)) {
                DEBUG('F', "Liberando %u por un error\n", sector);
                freeMap->Clear(sector);
            }
        } else {
            success = true;
            DEBUG('F', "Guardo las estructuras en %u\n", sector);
            synchDisk->ClearSector(sector);
            header->WriteBack(sector);
            freeMap->WriteBack(freeMapFile);
            DEBUG('F', "Actualizo el directorio padre\n");
            if (dir_sector == DIRECTORY_SECTOR) {
                directory->WriteBack(directoryFile);
            } else {
                OpenFile * f = new OpenFile(dir_sector);
                directory->WriteBack(f);
                delete f;
            }
        }
        delete header;
    }
    delete freeMap;
    delete directory;

    return success;
} // FileSystem::MakeDir

bool
FileSystem::RemoveDir(const char * _path)
{
    ASSERT(_path);

    const char * path = CheckRoot(_path);

    if (strcmp(path, "/") == 0) {
        return false;
    }

    int dir_sector = -1, folder_sector;
    Bitmap * freeMap;
    const char * name     = getName(path);
    Directory * directory = OpenPath(path, &dir_sector);

    DEBUG('F', "Eliminando el directorio %s y su contenido\n", path);
    freeMap = new Bitmap(NUM_SECTORS);
    freeMap->FetchFrom(freeMapFile);

    folder_sector = directory->Remove(name);

    if (folder_sector) {
        Directory * folder     = new Directory(NUM_DIR_ENTRIES);
        OpenFile * folder_file = new OpenFile(folder_sector);
        folder->FetchFrom(folder_file);
        folder->Clean(freeMap);
        FileHeader * header = new FileHeader;
        header->FetchFrom(folder_sector);
        header->Deallocate(freeMap);
        freeMap->Clear(folder_sector);
        delete header;
    }

    // Flush to disk.
    if (dir_sector == DIRECTORY_SECTOR) {
        directory->WriteBack(directoryFile);
    } else {
        OpenFile * f = new OpenFile(dir_sector);
        directory->WriteBack(f);
        delete f;
    }
    freeMap->WriteBack(freeMapFile);

    delete freeMap;
    delete directory;

    return true;
} // FileSystem::RemoveDir

bool
FileSystem::CheckPath(const char * _path)
{
    ASSERT(_path != nullptr);

    const char * path = CheckRoot(_path);

    int _sec = 0;
    Directory * directory = OpenPath(path, &_sec);

    if (directory != nullptr) {
        delete directory;
        return true;
    }

    return false;
}
