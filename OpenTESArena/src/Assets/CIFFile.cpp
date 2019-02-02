#include <algorithm>
#include <unordered_map>

#include "CIFFile.h"
#include "Compression.h"
#include "../Utilities/Bytes.h"
#include "../Utilities/Debug.h"

#include "components/vfs/manager.hpp"

namespace
{
	// These CIF files are headerless with a hardcoded frame count and pair
	// of dimensions (they seem to all be tile-based).
	const std::unordered_map<std::string, std::pair<int, Int2>> RawCifOverride =
	{
		{ "BRASS.CIF", { 9, Int2(8, 8) } },
		{ "BRASS2.CIF", { 9, Int2(8, 8) } },
		{ "MARBLE.CIF", { 9, Int2(3, 3) } },
		{ "MARBLE2.CIF", { 9, Int2(3, 3) } },
		{ "PARCH.CIF", { 9, Int2(20, 20) } },
		{ "SCROLL.CIF", { 9, Int2(20, 20) } }
	};
}

CIFFile::CIFFile(const std::string &filename)
{
	// Some filenames (i.e., Arrows.cif) have different casing between the floppy version and
	// CD version, so this needs to use the case-insensitive open() method for correct behavior
	// on Unix-based systems.
	VFS::IStreamPtr stream = VFS::Manager::get().openCaseInsensitive(filename);
	DebugAssertMsg(stream != nullptr, "Could not open \"" + filename + "\".");

	stream->seekg(0, std::ios::end);
	std::vector<uint8_t> srcData(stream->tellg());
	stream->seekg(0, std::ios::beg);
	stream->read(reinterpret_cast<char*>(srcData.data()), srcData.size());

	// X and Y offset might be useful for weapon positions on the screen.
	uint16_t xoff, yoff, width, height, flags, len;

	// Read header data if it is not a raw file.
	const auto rawOverride = RawCifOverride.find(filename);
	const bool isRaw = rawOverride != RawCifOverride.end();
	if (isRaw)
	{
		xoff = 0;
		yoff = 0;

		const Int2 &dims = rawOverride->second.second;
		width = dims.x;
		height = dims.y;

		flags = 0;
		len = width * height;
	}
	else
	{
		xoff = Bytes::getLE16(srcData.data());
		yoff = Bytes::getLE16(srcData.data() + 2);
		width = Bytes::getLE16(srcData.data() + 4);
		height = Bytes::getLE16(srcData.data() + 6);
		flags = Bytes::getLE16(srcData.data() + 8);
		len = Bytes::getLE16(srcData.data() + 10);
	}

	const int headerSize = 12;

	if ((flags & 0x00FF) == 0x0002)
	{		
		// Type 2 CIF.
		int offset = 0;

		while ((srcData.begin() + offset) < srcData.end())
		{
			const uint8_t *header = srcData.data() + offset;
			xoff = Bytes::getLE16(header);
			yoff = Bytes::getLE16(header + 2);
			width = Bytes::getLE16(header + 4);
			height = Bytes::getLE16(header + 6);
			flags = Bytes::getLE16(header + 8);
			len = Bytes::getLE16(header + 10);

			std::vector<uint8_t> decomp(width * height);
			Compression::decodeRLE(header + 12, width * height, decomp);

			this->pixels.push_back(std::make_unique<uint8_t[]>(width * height));
			this->offsets.push_back(Int2(xoff, yoff));
			this->dimensions.push_back(Int2(width, height));

			const uint8_t *srcPixels = decomp.data();
			uint8_t *dstPixels = this->pixels.back().get();
			std::copy(srcPixels, srcPixels + (width * height), dstPixels);

			offset += headerSize + len;
		}
	}
	else if ((flags & 0x00FF) == 0x0004)
	{
		// Type 4 CIF.
		int offset = 0;

		while ((srcData.begin() + offset) < srcData.end())
		{
			const uint8_t *header = srcData.data() + offset;
			xoff = Bytes::getLE16(header);
			yoff = Bytes::getLE16(header + 2);
			width = Bytes::getLE16(header + 4);
			height = Bytes::getLE16(header + 6);
			flags = Bytes::getLE16(header + 8);
			len = Bytes::getLE16(header + 10);

			std::vector<uint8_t> decomp(width * height);
			Compression::decodeType04(header + 12, header + 12 + len, decomp);

			this->pixels.push_back(std::make_unique<uint8_t[]>(width * height));
			this->offsets.push_back(Int2(xoff, yoff));
			this->dimensions.push_back(Int2(width, height));

			const uint8_t *srcPixels = decomp.data();
			uint8_t *dstPixels = this->pixels.back().get();
			std::copy(srcPixels, srcPixels + (width * height), dstPixels);

			offset += headerSize + len;
		}
	}
	else if ((flags & 0x00FF) == 0x0008)
	{
		// Type 8 CIF.
		int offset = 0;

		while ((srcData.begin() + offset) < srcData.end())
		{
			const uint8_t *header = srcData.data() + offset;
			xoff = Bytes::getLE16(header);
			yoff = Bytes::getLE16(header + 2);
			width = Bytes::getLE16(header + 4);
			height = Bytes::getLE16(header + 6);
			flags = Bytes::getLE16(header + 8);
			len = Bytes::getLE16(header + 10);

			std::vector<uint8_t> decomp(width * height);

			// Contains a 2 byte decompressed length after the header, so skip that 
			// (should be equivalent to width * height).
			Compression::decodeType08(header + 12 + 2, header + 12 + len, decomp);

			this->pixels.push_back(std::make_unique<uint8_t[]>(width * height));
			this->offsets.push_back(Int2(xoff, yoff));
			this->dimensions.push_back(Int2(width, height));

			const uint8_t *srcPixels = decomp.data();
			uint8_t *dstPixels = this->pixels.back().get();
			std::copy(srcPixels, srcPixels + (width * height), dstPixels);

			// Skip to the next image header.
			offset += headerSize + len;
		}
	}
	else if (isRaw)
	{
		// Uncompressed raw CIF.
		const int imageCount = rawOverride->second.first;

		for (int i = 0; i < imageCount; i++)
		{
			this->pixels.push_back(std::make_unique<uint8_t[]>(width * height));
			this->offsets.push_back(Int2(xoff, yoff));
			this->dimensions.push_back(Int2(width, height));

			const uint8_t *srcPixels = srcData.data() + (i * len);
			uint8_t *dstPixels = this->pixels.back().get();
			std::copy(srcPixels, srcPixels + len, dstPixels);
		}
	}
	else if ((flags & 0x00FF) == 0)
	{
		// Uncompressed CIF with headers.
		int offset = 0;

		// Read uncompressed images until the end of the file.
		while ((srcData.begin() + offset) < srcData.end())
		{
			const uint8_t *header = srcData.data() + offset;
			xoff = Bytes::getLE16(header);
			yoff = Bytes::getLE16(header + 2);
			width = Bytes::getLE16(header + 4);
			height = Bytes::getLE16(header + 6);
			flags = Bytes::getLE16(header + 8);
			len = Bytes::getLE16(header + 10);

			this->pixels.push_back(std::make_unique<uint8_t[]>(width * height));
			this->offsets.push_back(Int2(xoff, yoff));
			this->dimensions.push_back(Int2(width, height));

			const uint8_t *srcPixels = header + headerSize;
			uint8_t *dstPixels = this->pixels.back().get();
			std::copy(srcPixels, srcPixels + len, dstPixels);

			// Skip to the next image header.
			offset += headerSize + len;
		}
	}
	else
	{
		DebugCrash("Unrecognized flags " + std::to_string(flags) + ".");
	}
}

int CIFFile::getImageCount() const
{
	return static_cast<int>(this->pixels.size());
}

int CIFFile::getXOffset(int index) const
{
	return this->offsets.at(index).x;
}

int CIFFile::getYOffset(int index) const
{
	return this->offsets.at(index).y;
}

int CIFFile::getWidth(int index) const
{
	return this->dimensions.at(index).x;
}

int CIFFile::getHeight(int index) const
{
	return this->dimensions.at(index).y;
}

const uint8_t *CIFFile::getPixels(int index) const
{
	return this->pixels.at(index).get();
}
