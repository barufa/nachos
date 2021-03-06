/// Routines to manage an open Nachos file.  As in UNIX, a file must be open
/// before we can read or write to it.  Once we are all done, we can close it
/// (in Nachos, by deleting the `OpenFile` data structure).
///
/// Also as in UNIX, for convenience, we keep the file header in memory while
/// the file is open.
///
/// Copyright (c) 1992-1993 The Regents of the University of California.
///               2016-2017 Docentes de la Universidad Nacional de Rosario.
/// All rights reserved.  See `copyright.h` for copyright notice and
/// limitation of liability and disclaimer of warranty provisions.

#include "open_file.hh"
#include "file_header.hh"
#include "threads/system.hh"


/// Open a Nachos file for reading and writing.  Bring the file header into
/// memory while the file is open.
///
/// * `sector` is the location on disk of the file header for this file.
OpenFile::OpenFile(int _sector)
{
    DEBUG('O',"Creating OpenFile for sector:%d\n",_sector);
    sector = _sector;
    hdr    = new FileHeader;
    hdr->FetchFrom(sector);
    seekPosition = 0;
}

/// Close a Nachos file, de-allocating any in-memory data structures.
OpenFile::~OpenFile()
{
    DEBUG('O',"Deleting OpenFile for sector:%d\n",sector);
    #ifdef FILESYS
    Filenode * node = filetable->find(sector);
    if (node != nullptr) {
        node->users--;
        if (node->remove && node->users <= 0) {
            DEBUG('O',"Removing file:%s\n",node->name);
            fileSystem->Remove(node->name);
        }
    }
    #endif
    delete hdr;
}

/// Change the current location within the open file -- the point at which
/// the next `Read` or `Write` will start from.
///
/// * `position` is the location within the file for the next `Read`/`Write`.
void
OpenFile::Seek(unsigned position)
{
    seekPosition = position;
}

/// OpenFile::Read/Write
///
/// Read/write a portion of a file, starting from `seekPosition`.  Return the
/// number of bytes actually written or read, and as a side effect, increment
/// the current position within the file.
///
/// Implemented using the more primitive `ReadAt`/`WriteAt`.
///
/// * `into` is the buffer to contain the data to be read from disk.
/// * `from` is the buffer containing the data to be written to disk.
/// * `numBytes` is the number of bytes to transfer.

int
OpenFile::Read(char * into, unsigned numBytes)
{
    ASSERT(into != nullptr);
    ASSERT(numBytes > 0);

    int result = ReadAt(into, numBytes, seekPosition);
    seekPosition += result;
    return result;
}

int
OpenFile::Write(const char * into, unsigned numBytes)
{
    ASSERT(into != nullptr);
    ASSERT(numBytes > 0);

    int result = WriteAt(into, numBytes, seekPosition);
    seekPosition += result;

    return result;
}

/// OpenFile::ReadAt/WriteAt
///
/// Read/write a portion of a file, starting at `position`.  Return the
/// number of bytes actually written or read, but has no side effects (except
/// that `Write` modifies the file, of course).
///
/// There is no guarantee the request starts or ends on an even disk sector
/// boundary; however the disk only knows how to read/write a whole disk
/// sector at a time.  Thus:
///
/// For ReadAt:
///     We read in all of the full or partial sectors that are part of the
///     request, but we only copy the part we are interested in.
/// For WriteAt:
///     We must first read in any sectors that will be partially written, so
///     that we do not overwrite the unmodified portion.  We then copy in the
///     data that will be modified, and write back all the full or partial
///     sectors that are part of the request.
///
/// * `into` is the buffer to contain the data to be read from disk.
/// * `from` is the buffer containing the data to be written to disk.
/// * `numBytes` is the number of bytes to transfer.
/// * `position` is the offset within the file of the first byte to be
///   read/written.

int
OpenFile::ReadAt(char * into, unsigned numBytes, unsigned position)
{
    DEBUG('O',"Inside ReadAt\n");

    Filenode * node = filetable->find(sector);


    hdr->FetchFrom(sector);

    if (node != nullptr) {
        DEBUG('O',"Waiting for read %s\n",node->name);
        node->Can_Read->P();
        node->lectores++;
        if (node->lectores == 1){ // Bloqueo escritura
            DEBUG('O',"Disabling writing\n",node->name);
            node->Can_Write->P();
        }
        node->Can_Read->V();
    }

    int ret = Internal_ReadAt(into, numBytes, position);
    if (node != nullptr) {
        node->Can_Read->P();
        node->lectores--;
        if (node->lectores == 0){ // Desbloqueo escritura
            DEBUG('O',"Enabling writing\n",node->name);
            node->Can_Write->V();
        }
        node->Can_Read->V();
    }

    DEBUG('O',"Leaving ReadAt\n");

    return ret;
}

int
OpenFile::WriteAt(const char * from, unsigned numBytes, unsigned position)
{
    DEBUG('O',"Inside WriteAt\n");
    if (Length() < position + numBytes) {
        unsigned size = (position + numBytes) - Length() + 1;
        if (!fileSystem->Expand(sector, size)) {
            numBytes = Length() - position;
        }
    }
    ASSERT(position + numBytes <= Length());

    Filenode * node = filetable->find(sector);
    hdr->FetchFrom(sector);

    if (node != nullptr) {
        DEBUG('O',"Waiting for write %s\n",node->name);
        node->Can_Write->P();
    }
    int ret = Internal_WriteAt(from, numBytes, position);

    if (node != nullptr) {
        DEBUG('O',"Leaving writing of %s\n",node->name);
        node->Can_Write->V();
    }

    DEBUG('O',"Leaving ReadAt\n");

    return ret;
}

int
OpenFile::Internal_ReadAt(char * into, unsigned numBytes, unsigned position)
{
    ASSERT(into != nullptr);
    ASSERT(numBytes >= 0);

    if (numBytes == 0) {
        return numBytes;
    }

    unsigned fileLength = hdr->FileLength();
    unsigned firstSector, lastSector, numSectors;
    char * buf;

    if (position >= fileLength)
        return 0;  // Check request.

    if (position + numBytes > fileLength)
        numBytes = fileLength - position;
    DEBUG('O', "Reading %u bytes at %u, from file of length %u.\n",
      numBytes, position, fileLength);

    firstSector = DivRoundDown(position, SECTOR_SIZE);
    lastSector  = DivRoundDown(position + numBytes - 1, SECTOR_SIZE);
    numSectors  = 1 + lastSector - firstSector;

    // Read in all the full and partial sectors that we need.
    buf = new char [numSectors * SECTOR_SIZE];
    for (unsigned i = firstSector; i <= lastSector; i++) {
        synchDisk->ReadSector(hdr->ByteToSector(i * SECTOR_SIZE),
          &buf[(i - firstSector) * SECTOR_SIZE]);
    }
    // Copy the part we want.
    memcpy(into, &buf[position - firstSector * SECTOR_SIZE], numBytes);
    delete [] buf;
    return numBytes;
} // OpenFile::Internal_ReadAt

int
OpenFile::Internal_WriteAt(const char * from, unsigned numBytes,
  unsigned position)
{
    ASSERT(from != nullptr);
    ASSERT(numBytes > 0);

    unsigned fileLength = hdr->FileLength();
    unsigned firstSector, lastSector, numSectors;
    bool firstAligned, lastAligned;
    char * buf;

    if (position >= fileLength)
        return 0;  // Check request.

    if (position + numBytes > fileLength) {
        unsigned add_size = numBytes - (fileLength - position);
        if (!fileSystem->Expand(sector, add_size)) {
            numBytes = fileLength - position;
        }
    }
    DEBUG('O', "Writing %u bytes at %u, from file of length %u.\n",
      numBytes, position, fileLength);

    firstSector = DivRoundDown(position, SECTOR_SIZE);
    lastSector  = DivRoundDown(position + numBytes - 1, SECTOR_SIZE);
    numSectors  = 1 + lastSector - firstSector;

    buf = new char [numSectors * SECTOR_SIZE];

    firstAligned = position == firstSector * SECTOR_SIZE;
    lastAligned  = position + numBytes == (lastSector + 1) * SECTOR_SIZE;

    // Read in first and last sector, if they are to be partially modified.
    if (!firstAligned)
        Internal_ReadAt(buf, SECTOR_SIZE, firstSector * SECTOR_SIZE);
    if (!lastAligned && (firstSector != lastSector || firstAligned)) {
        Internal_ReadAt(&buf[(lastSector - firstSector) * SECTOR_SIZE],
          SECTOR_SIZE, lastSector * SECTOR_SIZE);
    }

    // Copy in the bytes we want to change.
    memcpy(&buf[position - firstSector * SECTOR_SIZE], from, numBytes);

    // Write modified sectors back.
    for (unsigned i = firstSector; i <= lastSector; i++) {
        synchDisk->WriteSector(hdr->ByteToSector(i * SECTOR_SIZE),
          &buf[(i - firstSector) * SECTOR_SIZE]);
    }
    delete [] buf;
    return numBytes;
} // OpenFile::WriteAt

/// Return the number of bytes in the file.
unsigned
OpenFile::Length() const
{
    hdr->FetchFrom(sector);
    return hdr->FileLength();
}

unsigned
OpenFile::Get_Sector() const
{
    return sector;
}
