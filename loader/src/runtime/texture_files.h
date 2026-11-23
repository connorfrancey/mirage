#pragma once
/** @file
 * @ingroup mirage_assets
 * @brief Offline texture conversion and cooked package overlays.
 */
// Texture replacement, the file side (Mirage, step 7): what a mod's image becomes in the game's package format. Pure:
// no engine, no Windows; unit-tested (tests/test_textures.cpp).
//
// How the game stores a texture, measured on the Prologue and menu maps (9,157 textures):
//   - a map holds a *copy* of each texture it uses ("B_C_01.Facade.T_X", outer chain kept). Streamable copies keep
//     their small mips inline and stream the large ones ("S4i7": 4 separate, 7 inline) from the texture's own package
//     (B_C_01.upk) by raw file offset; those storage packages are not compressed;
//   - every texture whose mips are all inline is NeverStream or small enough to stay resident.
// So a replaced texture is written as NeverStream with all its mips inline, into every package that has a copy of it
// (BuildOverlay). Storage packages are never rewritten, and no other package's streamed mips move.
//
// Static meshes are replaced the same way (step 31): maps hold copies of them too, and a replacement is a mod's model
// file (glTF, FBX or OBJ) converted by `mirage model <file> --convert` (runtime/mesh_file.h), written in place of every
// copy (runtime/model_files.h's layout), keeping the original's materials (section by section) and its properties,
// with UseSimpleLineCollision and UseSimpleBoxCollision off: the player collides with the new triangles. Its
// RB_BodySetup subobject stays, its properties too, but its shape becomes the model's (models::BodyGeometry; no cooked
// copy, PhysX cooks it when a body is made), so physics props of it are the new shape.
// A skeletal mesh is replaced by a skinned model the same way (models::MakeSkeletalMesh): the original's skeleton, so
// its animations play on the model, and its materials and properties.

#include <UnrealLib/Assets/Texture.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mirage::textures
{
	using UnrealLib::Assets::PixelFormat;

	struct Mip
	{
		int width = 0, height = 0;
		std::vector<std::byte> data;	// In the texture's format (DXT blocks, BGRA, ...)
	};
	struct Encoded
	{
		PixelFormat format = PixelFormat::Unknown;
		std::vector<Mip> mips;			// Largest first, down to 1x1 for a generated chain
	};

	// Formats the encoder writes: DXT1, DXT3, DXT5, A8R8G8B8, G8 (every format the game's textures use).
	bool CanEncode(PixelFormat format);

	// A mod's replacement image: .dds is taken as stored (DXT1/3/5, 32-bit ARGB or 8-bit luminance, its own mips);
	// .png / .tga / .bmp / .jpg are decoded, given a full mip chain (box filter) and encoded to `target`, the replaced
	// texture's own format. An image whose sides aren't powers of two is resampled to the nearest (a DDS must be one).
	// Throws std::runtime_error saying what is wrong.
	/// @brief Prepare a mod image for texture replacement.
	/// @param file Image file. DDS keeps its stored format and mip levels.
	/// @param target Format used to encode decoded source images.
	/// @return Owned encoded mip chain.
	/// @throws std::runtime_error When the image cannot be read or encoded.
	Encoded LoadReplacement(const std::filesystem::path& file, PixelFormat target);
	// What EncodeImage does to an image's pixels first (not to a DDS, which is taken as stored)
	enum class Pixels
	{
		AsStored,
		FlipGreen,		// A normal map in OpenGL's convention (glTF's: green up) turned to the game's (DirectX's: green down)
		Gloss,			// glTF's metallic-roughness image turned to a grey specular map: 1 - roughness (its green)
	};
	// An image file's bytes as LoadReplacement reads them (`dds`: taken as stored), encoded to `target`.
	Encoded EncodeImage(const std::vector<std::byte>& file, bool dds, PixelFormat target, Pixels pixels = Pixels::AsStored);
	// A Texture2D's native data holding `encoded` (version 536: empty source art, then every mip inline and
	// uncompressed). `base`: the package offset of its first byte (bulk data stores absolute offsets).
	std::vector<std::byte> TextureData(const UnrealLib::SerializationContext& ctx, const Encoded& encoded, std::uint64_t base);

	// One surface from 8-bit RGBA (width x height, rows packed), in `format`. Block formats pad partial blocks.
	std::vector<std::byte> EncodeSurface(const std::uint8_t* rgba, int width, int height, PixelFormat format);
	// A full mip chain from RGBA, encoded (sides powers of two).
	Encoded EncodeChain(const std::uint8_t* rgba, int width, int height, PixelFormat format);
	// A DDS file's surfaces (throws std::runtime_error for anything else).
	Encoded ParseDds(const std::vector<std::byte>& file);

	// The lowercase paths of replaceable textures, meshes and SoundNodeWave exports from the package tables. A copy
	// keeps its original path; an object of the package itself gets the package's name in front
	// ("edge_p.lightmaps.lm_1").
	std::vector<std::string> ScanReplaceable(const std::filesystem::path& package);

	// The lowercase object path a mod names an asset by.
	std::string NormalizePath(std::string path);

	// A model file (.glb, .gltf, .fbx, .obj): a mesh's replacement, read from its converted <file>.mmesh
	bool IsModelFile(const std::filesystem::path& file);
	bool IsSoundFile(const std::filesystem::path& file);

	struct Replacement
	{
		std::string path;					// Lowercase object path (NormalizePath)
		std::filesystem::path file;			// The mod's image, model or Ogg Vorbis file
	};
	struct Overlay
	{
		std::vector<std::byte> bytes;		// The rewritten package (LZO-compressed like the original)
		std::vector<std::string> replaced;	// Paths written
		std::vector<std::string> problems;	// Replacements that couldn't be used, and why
	};
	// `package` with every replacement it holds a copy of rewritten (textures: NeverStream, all mips inline, sizes and
	// format from the image; meshes: the converted model; sound waves: the Ogg Vorbis stream and its metadata).
	// `compress`: LZO like the original (slow: seconds per map), else uncompressed (the engine reads both; the game's
	// own storage packages are uncompressed).
	/// @brief Build an in-memory package overlay with matching exports replaced.
	/// @param package Cooked package to read. The file remains untouched.
	/// @param replacements Requested textures, meshes or sound waves.
	/// @param compress Write LZO-compressed package data when true.
	/// @return Owned package bytes and diagnostics for replacements that failed.
	/// @throws std::runtime_error When the package cannot be read or serialized.
	Overlay BuildOverlay(const std::filesystem::path& package, const std::vector<Replacement>& replacements,
						 bool compress = false);
}
