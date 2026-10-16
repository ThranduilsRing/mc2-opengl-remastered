#include "ablscan.h"
#include "../GameOS/gameos/utils/mc2log.h"

long (*ABLFile::createCB) (void** file, const char* fName) = nullptr;
long (*ABLFile::openCB) (void** file, const char* fName) = nullptr;
long (*ABLFile::closeCB) (void** file) = nullptr;
bool (*ABLFile::eofCB) (void* file) = nullptr;
long (*ABLFile::remainingCB) (void* file) = nullptr;
long (*ABLFile::readCB) (void* file, unsigned char* buffer, long length) = nullptr;
int32_t (*ABLFile::readIntCB) (void* file) = nullptr;
long (*ABLFile::readLongCB) (void* file) = nullptr;
long (*ABLFile::readStringCB) (void* file, unsigned char* buffer) = nullptr;
long (*ABLFile::readLineExCB) (void* file, unsigned char* buffer, long maxLength) = nullptr;
long (*ABLFile::writeCB) (void* file, unsigned char* buffer, long length) = nullptr;
long (*ABLFile::writeByteCB) (void* file, unsigned char byte) = nullptr;
long (*ABLFile::writeIntCB) (void* file, int32_t val) = nullptr;
long (*ABLFile::writeLongCB) (void* file, long value) = nullptr;
long (*ABLFile::writeStringCB) (void* file, const char* buffer) = nullptr;

void ABLFile::init (void) {

	fileName.clear();
	file = nullptr;
}

//-----------------------------------------------------------------------------
	
void ABLFile::destroy (void) noexcept {

	close();
	fileName.clear();
}

//---------------------------------------------------------------------------

long ABLFile::create (const char* fName) {

	close();
	fileName.clear();
	if (fName) {
		fileName = fName;
		const long result = createCB(&file, fileName.c_str());
		if (result != ABL_NO_ERR)
			MC2_LOG(Sim, Error, "[ABL_FILE_OPEN_BAD] cannot create {} ({})", fileName, result);
		return result;
	}
	return(-1);
}

//---------------------------------------------------------------------------

long ABLFile::open (const char* fName) {

	close();
	fileName.clear();
	if (fName) {
		fileName = fName;
		const long result = openCB(&file, fileName.c_str());
		if (result != ABL_NO_ERR)
			MC2_LOG(Sim, Error, "[ABL_FILE_OPEN_BAD] cannot open {} ({})", fileName, result);
		return result;
	}
	return(-1);
}

//---------------------------------------------------------------------------

long ABLFile::close (void) {

	if (!file)
		return(-1);
	long result = closeCB(&file);
	file = nullptr;
	return result;
}

//-----------------------------------------------------------------------------

bool ABLFile::eof (void) {

	if (file)
		return(eofCB(file));
	return(true);
}

//-----------------------------------------------------------------------------

long ABLFile::read (unsigned char* buffer, long length) {

	if (!file || length < 0 || !remainingCB || length > remainingCB(file)) {
		MC2_LOG(Sim, Error, "[ABL_FILE_READ_BAD] invalid or truncated read of {} bytes from {}", length, fileName);
		return 0;
	}
	const long count = readCB(file, buffer, length);
	if (count != length) {
		MC2_LOG(Sim, Error, "[ABL_FILE_READ_BAD] short read of {} bytes from {}", length, fileName);
		return 0;
	}
	return count;
}

//-----------------------------------------------------------------------------

int32_t ABLFile::readInt (void) {

	int32_t value = -1;
	if (read(reinterpret_cast<unsigned char*>(&value), sizeof(value)) != sizeof(value))
		return -1;
	return value;
}

//-----------------------------------------------------------------------------

long ABLFile::readLong (void) {

	return readInt();
}

//-----------------------------------------------------------------------------

long ABLFile::readString (unsigned char* buffer, long capacity) {

	if (buffer && capacity > 0)
		buffer[0] = 0;
	if (file && buffer && capacity > 0 && remainingCB && readCB) {
		for (long count = 0; count < capacity && remainingCB(file) > 0; ++count) {
			unsigned char ch;
			if (readCB(file, &ch, 1) != 1)
				break;
			if (ch == 0) {
				buffer[count] = 0;
				return count;
			}
			if (count == capacity - 1)
				break;
			buffer[count] = ch;
			buffer[count + 1] = 0;
		}
	}
	MC2_LOG(Sim, Error, "[ABL_FILE_STRING_BAD] truncated or oversized string from {}", fileName);
	return -1;
}

//-----------------------------------------------------------------------------

long ABLFile::readLineEx (unsigned char* buffer, long maxLength) {

	if (buffer && maxLength > 0)
		buffer[0] = 0;
	if (file && buffer && maxLength > 0 && remainingCB && readCB) {
		long count = 0;
		while (remainingCB(file) > 0 && count < maxLength - 1) {
			unsigned char ch;
			if (readCB(file, &ch, 1) != 1)
				break;
			buffer[count++] = ch;
			buffer[count] = 0;
			if (ch == '\n')
				return count + 1;
		}
		if (remainingCB(file) == 0)
			return count ? count + 1 : 0;
	}
	MC2_LOG(Sim, Error, "[ABL_FILE_LINE_BAD] truncated or oversized line from {}", fileName);
	return 0;
}

//-----------------------------------------------------------------------------

long ABLFile::write (unsigned char* buffer, long length) {

	if (file)
		return(writeCB(file, buffer, length));
	return(0);
}

//-----------------------------------------------------------------------------

long ABLFile::writeByte (unsigned char val) {

	if (file)
		return(writeByteCB(file, val));
	return(0);
}

//-----------------------------------------------------------------------------

long ABLFile::writeInt (int32_t val) {

	if (file)
		return(writeIntCB(file, val));
	return(0);
}

//-----------------------------------------------------------------------------

long ABLFile::writeLong (long val) {

	if (file)
		return(writeLongCB(file, val));
	return(0);
}

//-----------------------------------------------------------------------------

long ABLFile::writeString (const char* buffer) {

	if (file)
		return(writeStringCB(file, buffer));
	return(0);
}

