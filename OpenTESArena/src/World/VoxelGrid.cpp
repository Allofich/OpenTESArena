#include <algorithm>

#include "VoxelGrid.h"

#include "components/debug/Debug.h"

VoxelGrid::VoxelGrid(int width, int height, int depth)
{
	const int voxelCount = width * height * depth;
	this->voxels = std::vector<uint16_t>(voxelCount, 0);

	this->width = width;
	this->height = height;
	this->depth = depth;
}

int VoxelGrid::getIndex(int x, int y, int z) const
{
	DebugAssert(x >= 0);
	DebugAssert(y >= 0);
	DebugAssert(z >= 0);
	DebugAssert(x < this->width);
	DebugAssert(y < this->height);
	DebugAssert(z < this->depth);
	return x + (y * this->width) + (z * this->width * this->height);
}

Int2 VoxelGrid::getTransformedCoordinate(const Int2 &voxel, int gridWidth, int gridDepth)
{
	// These have a -1 whereas the Double2 version does not since all .MIF start points
	// are in the center of a voxel, giving a minimum distance of 0.5 from grid sides,
	// thus guaranteeing that no out-of-bounds grid access will occur in those cases.
	// This one doesn't have that bias (due to integers), and as such, values may go out 
	// of grid range if using the unmodified dimensions.
	return Int2((gridWidth - 1) - voxel.y, (gridDepth - 1) - voxel.x);
}

Double2 VoxelGrid::getTransformedCoordinate(const Double2 &voxel, int gridWidth, int gridDepth)
{
	return Double2(
		static_cast<double>(gridWidth) - voxel.y,
		static_cast<double>(gridDepth) - voxel.x);
}

int VoxelGrid::getWidth() const
{
	return this->width;
}

int VoxelGrid::getHeight() const
{
	return this->height;
}

int VoxelGrid::getDepth() const
{
	return this->depth;
}

uint16_t *VoxelGrid::getVoxels()
{
	return this->voxels.data();
}

const uint16_t *VoxelGrid::getVoxels() const
{
	return this->voxels.data();
}

uint16_t VoxelGrid::getVoxel(int x, int y, int z) const
{
	const int index = this->getIndex(x, y, z);
	return this->voxels.data()[index];
}

VoxelData &VoxelGrid::getVoxelData(uint16_t id)
{
	return this->voxelData.at(id);
}

const VoxelData &VoxelGrid::getVoxelData(uint16_t id) const
{
	return this->voxelData.at(id);
}

uint16_t VoxelGrid::addVoxelData(const VoxelData &voxelData)
{
	this->voxelData.push_back(voxelData);

	return static_cast<uint16_t>(this->voxelData.size() - 1);
}

void VoxelGrid::setVoxel(int x, int y, int z, uint16_t id)
{
	const int index = this->getIndex(x, y, z);
	this->voxels.data()[index] = id;
}
