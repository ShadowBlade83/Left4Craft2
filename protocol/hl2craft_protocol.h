// HL2Minecraft shared-memory protocol (Half-Life 2 <-> Minecraft Fabric mod).
// Single source of truth for byte layout. Mirrored in fabric/src/main/java/dev/hl2craft/link/Proto.java.
#pragma once

#include <cstdint>

namespace hl2craft::proto
{
	inline constexpr std::uint32_t kMagic = 0x43324C48;  // "HL2C"
	inline constexpr std::uint32_t kVersion = 14;
	inline constexpr wchar_t       kMappingName[] = L"Local\\HL2Minecraft_v1";

	// 1 Minecraft block == 1.0 metre == 39.37007874 Source units (1 Source unit = 0.0254 m = 1 inch).
	inline constexpr double kUnitsPerBlock = 39.37007874015748;
	inline constexpr double kMetersPerUnit = 0.0254;

	// ---- region offsets ---------------------------------------------------------------------
	inline constexpr std::uint64_t kOffHeader = 0x0;
	inline constexpr std::uint64_t kOffHostState = 0x100;
	inline constexpr std::uint64_t kOffMcState = 0x200;
	inline constexpr std::uint64_t kOffOverlayCtl = 0x300;
	inline constexpr std::uint64_t kOffOverlaySlotHdr = 0x340;  // 3 x 0x40
	inline constexpr std::uint64_t kOffInputRing = 0x1000;
	inline constexpr std::uint64_t kOffActorTable = 0x12000;   // Host -> MC, see ActorTable
	inline constexpr std::uint64_t kOffEventRing = 0x17000;    // MC -> Host, see McEvent
	inline constexpr std::uint64_t kOffWorldEntities = 0x1C000;// MC -> Host, see WorldEntities
	inline constexpr std::uint64_t kOffCollisionRing = 0x20000;
	inline constexpr std::uint64_t kCollisionRingBytes = 32ull << 20; // 32 MB
	inline constexpr std::uint64_t kOffOverlayPixels = kOffCollisionRing + kCollisionRingBytes;
	inline constexpr std::uint32_t kMaxOverlayW = 3840;
	inline constexpr std::uint32_t kMaxOverlayH = 2160;
	inline constexpr std::uint64_t kOverlaySlotBytes = std::uint64_t(kMaxOverlayW) * kMaxOverlayH * 4;
	inline constexpr std::uint32_t kOverlaySlots = 3;
	inline constexpr std::uint64_t kOffRenderRing = kOffOverlayPixels + kOverlaySlotBytes * kOverlaySlots;
	inline constexpr std::uint64_t kRenderRingBytes = 128ull << 20; // 128 MB
	inline constexpr std::uint64_t kOffFrameBuffer = kOffRenderRing;
	inline constexpr std::uint64_t kFrameBufferBytes = kRenderRingBytes;
	inline constexpr std::uint64_t kMappingBytes = kOffRenderRing + kRenderRingBytes;

	// ---- header @0x0 ------------------------------------------------------------------------
	struct Header
	{
		std::uint32_t magic;
		std::uint32_t version;
		std::uint32_t hostPid;
		std::uint32_t mcPid;
		std::uint64_t hostHeartbeatMs;  // GetTickCount64() at last HL2 frame
		std::uint64_t mcHeartbeatMs;    // GetTickCount64() at last MC frame
	};
	static_assert(sizeof(Header) == 0x20);

	// ---- HL2 -> MC state @0x100 (seqlock: seq odd while writing) ----------------------------
	enum HostFlags : std::uint32_t
	{
		kHostInGame    = 1u << 0,  // player is in playable level (not menu, not loading)
		kHostMenuOpen  = 1u << 1,  // pause / main menu open
		kHostLoading   = 1u << 2,  // loading screen / transition
		kHostPaused    = 1u << 3,  // game is paused
		kHostOnGround  = 1u << 4,  // player is touching solid ground in HL2
		kHostOnLadder  = 1u << 5,  // player is touching a ladder in HL2
		kHostVehicleOrOverride = 1u << 6, // player is in/near a vehicle or HL2 physics override active
	};

	struct HostState
	{
		std::uint32_t seq;
		std::uint32_t flags;           // HostFlags
		std::uint32_t mapId;           // hash of map name
		std::uint32_t collisionEpoch;  // bumps on level change; MC drops all collision
		double        posX, posY, posZ;// HL2 player feet in MC coordinates
		float         yaw, pitch;      // MC look degrees
		std::uint32_t teleportSeq;     // MC teleports its player to pos when this changes
		std::uint32_t viewportW, viewportH;
		float         fov;
		std::uint32_t gameMode;        // 0: Survival, 1: Creative
	};
	static_assert(sizeof(HostState) <= 0x100);

	// ---- MC -> HL2 state @0x200 (seqlock) ---------------------------------------------------
	enum McFlags : std::uint32_t
	{
		kMcInWorld    = 1u << 0,
		kMcScreenOpen = 1u << 1,  // inventory, chest, chat etc. is open
		kMcOnGround   = 1u << 2,
		kMcSneaking   = 1u << 3,
		kMcSprinting  = 1u << 4,
		kMcDead       = 1u << 5,
		kMcSwimming   = 1u << 6,
		kMcFlying     = 1u << 7,  // creative flight or elytra
		kMcBlocking   = 1u << 8,  // player blocking with shield
	};

	struct McState
	{
		std::uint32_t seq;
		std::uint32_t flags;          // McFlags
		double        x, y, z;        // interpolated feet position (MC coords)
		float         yaw, pitch;     // MC look degrees
		float         eyeHeight;      // blocks above feet
		float         sensitivity;    // MC mouse sensitivity option (0..1)
		std::uint32_t teleportAck;    // last HostState::teleportSeq applied
		std::uint32_t guiScale;
		std::uint64_t frameCounter;
		float         fovDeg;
		std::uint32_t gameMode;       // 0: Survival, 1: Creative
		std::uint32_t cameraMode;     // 0: 1st person, 1: 3rd person back, 2: 3rd person front
		float         cameraDistance; // Distance in MC blocks
		float         supportHeight;  // MC Y of highest solid block under player feet
		float         health;         // MC player health (0..20)
		std::uint32_t heldLight;      // MC held item light level (0..15)
		std::uint32_t heldLightType;  // 0: none, 1: warm, 2: soul
	};
	static_assert(sizeof(McState) <= 0x100);

	// ---- overlay triple buffer @0x300 --------------------------------------------------------
	inline constexpr std::uint32_t kOverlayDirty = 1u << 2;

	struct OverlayCtl
	{
		std::uint32_t state;
		std::uint32_t pad;
		std::uint64_t framesPublished;
	};

	struct OverlaySlotHdr
	{
		std::uint32_t width;
		std::uint32_t height;
		std::uint32_t flags;  // bit0: rows are bottom-up
		std::uint32_t pad;
		std::uint64_t frameId;
		std::uint8_t  reserved[0x40 - 0x18];
	};
	static_assert(sizeof(OverlaySlotHdr) == 0x40);

	// ---- input ring @0x1000 (HL2 produces, MC consumes) -------------------------------------
	inline constexpr std::uint32_t kInputRingEntries = 4096;
	inline constexpr std::uint64_t kInputRingHeadOff = 0x00;
	inline constexpr std::uint64_t kInputRingTailOff = 0x40;
	inline constexpr std::uint64_t kInputRingDataOff = 0x80;

	enum InputType : std::uint16_t
	{
		kInKey         = 1,  // code = keycode, a = 1 press / 0 release
		kInMouseButton = 2,  // code = button (1 L, 2 M, 3 R, 4 X1, 5 X2), a = 1 press / 0 release
		kInScroll      = 3,  // a = notches * 120 (positive = up)
		kInCursor      = 4,  // a, b = absolute cursor position in overlay pixels
		kInText        = 5,  // a = unicode code point
		kInReleaseAll  = 6,  // release every held key
		kInHurt        = 7,  // HL2 hit the player: a = damage, b = attacker entindex
		kInOpenMenu    = 8,  // open Minecraft pause/options menu
		kInBreakBlock  = 100, // L4D2: the infected broke a barricade block, a b c = block x y z (Left4Craft)
	};

	struct InputEvent
	{
		std::uint16_t type;
		std::uint16_t code;
		std::int32_t  a;
		std::int32_t  b;
		std::int32_t  c;
	};
	static_assert(sizeof(InputEvent) == 16);

	// ---- actor table @0x12000 (HL2 -> MC, seqlock) ------------------------------------------
	inline constexpr std::uint32_t kMaxActors = 256;

	enum ActorFlags : std::uint32_t
	{
		kActorHostile   = 1u << 0,
		kActorDead      = 1u << 1,
		kActorFriend    = 1u << 2,
		kActorInCombat  = 1u << 3,
		kActorArmed     = 1u << 4,
	};

	struct ActorRecord
	{
		std::uint32_t entIndex;
		std::uint32_t flags;       // ActorFlags
		float         x, y, z;     // feet, MC coords
		float         yaw;         // MC degrees
		float         width;       // blocks
		float         height;      // blocks
		float         healthFrac;  // 0..1
		std::uint16_t classId;
		std::uint16_t pad;
		char          name[24];    // name or classname, UTF-8
	};
	static_assert(sizeof(ActorRecord) == 64);

	struct ActorTable
	{
		std::uint32_t seq;
		std::uint32_t count;
		std::uint8_t  pad[0x40 - 8];
		ActorRecord   actors[kMaxActors];
	};
	static_assert(sizeof(ActorTable) == 0x40 + 64 * kMaxActors);

	// ---- event ring @0x17000 (MC -> HL2) ----------------------------------------------------
	inline constexpr std::uint32_t kEventRingEntries = 512;
	inline constexpr std::uint64_t kEventRingHeadOff = 0x00;
	inline constexpr std::uint64_t kEventRingTailOff = 0x40;
	inline constexpr std::uint64_t kEventRingDataOff = 0x80;

	enum McEventType : std::uint32_t
	{
		kEvHitActor    = 1, // entIndex, a = MC damage, b/c = knockback x/z, d = knockback strength
		kEvPlayerDied  = 2, // player died in MC -> kill HL2 player
		kEvExplosion   = 3, // TNT/creeper explosion -> a,b,c center in MC coords, d = radius
		kEvTeleport    = 4, // Ender Pearl -> a,b,c destination in MC coords
		kEvBlockPlaced = 5, // placed block -> a,b,c block coords in MC
		kEvBlockBroken = 6, // broken block -> a,b,c block coords in MC
		kEvClearBlocks = 7, // clear all placed blocks -> level change or player died
	};

	enum HitWeapon : std::uint32_t
	{
		kWeaponUnarmed = 0,
		kWeaponBlade   = 1, // sword
		kWeaponAxe     = 2, // axe
		kWeaponPickaxe = 3, // pickaxe / crowbar
		kWeaponArrow   = 4, // bow / projectile
	};

	struct McEvent
	{
		std::uint32_t type;
		std::uint32_t entIndex;
		float         a, b, c, d;
		std::uint32_t flags;
		std::uint32_t weapon;
	};
	static_assert(sizeof(McEvent) == 32);

	// ---- world entities @0x1C000 (MC -> HL2, seqlock) -------------------------------------
	inline constexpr std::uint32_t kMaxWorldEntities = 160;

	enum WorldEntityKind : std::uint32_t
	{
		kWeArrow   = 1, // uv[0]: the arrow's item icon
		kWeItem    = 2, // dropped/thrown item: a flat sprite (uv[0]) turning about the vertical
		kWeTrident = 3, // uv[0]: the trident's item icon
		kWeBlock   = 4, // dropped block item: a spinning cube of side scale, uv[0..2] = side, top, bottom
		kWeCrack   = 5, // block-breaking cracks over the box at (x, y, z) of size ext, uv[0] = crack stage
		kWeShadow  = 6, // player's/mob's feet at (x, y, z)
		kWeMob     = 7, // living mob: Creeper, Zombie, Skeleton, Spider, animals, etc.
	};

	struct WorldEntity
	{
		std::uint32_t kind;        // WorldEntityKind
		std::uint32_t id;          // MC entity id
		float         x, y, z;     // MC coords (interpolated at MC render time)
		float         yaw, pitch;  // MC degrees
		float         scale;
		float         ext[3];      // kWeCrack: box size
		float         uv[3][4];    // atlas rects {u0, v0, u1, v1}
		std::uint32_t tint;        // RGBA8 multiplier for top face; 0 = none
	};
	static_assert(sizeof(WorldEntity) == 96);

	struct WorldEntities
	{
		std::uint32_t seq;
		std::uint32_t count;
		std::uint32_t hasSelection;          // draw outline around targeted block
		float         selMin[3], selMax[3];  // MC coords
		std::uint8_t  pad[0x40 - 36];
		WorldEntity   entities[kMaxWorldEntities];
	};
	static_assert(sizeof(WorldEntities) == 0x40 + sizeof(WorldEntity) * kMaxWorldEntities);
	static_assert(kOffWorldEntities + sizeof(WorldEntities) <= kOffCollisionRing);

	// ---- collision ring @0x20000 (HL2 produces, MC consumes) --------------------------------
	inline constexpr std::uint64_t kColRingHeadOff = 0x00;
	inline constexpr std::uint64_t kColRingTailOff = 0x40;
	inline constexpr std::uint64_t kColRingDataOff = 0x80;
	inline constexpr std::uint64_t kColRingDataBytes = kCollisionRingBytes - kColRingDataOff;

	enum ColType : std::uint32_t
	{
		kColPad    = 0,
		kColClear  = 1,   // payload: u32 epoch
		kColRegion = 2,   // payload: ColRegion + ColBlock[count]
		kColTris   = 3,   // payload: ColRegion + ColTri[count]
	};

	struct ColRegion
	{
		std::int32_t  minX, minY, minZ;
		std::int32_t  maxX, maxY, maxZ;
		std::uint32_t epoch;
		std::uint32_t count;
	};
	static_assert(sizeof(ColRegion) == 32);

	// 8x8x8 occupancy mask of sub-voxels (1/8 block each)
	struct ColBlock
	{
		std::int32_t  x, y, z;
		std::uint32_t pad;
		std::uint64_t bits[8];
	};
	static_assert(sizeof(ColBlock) == 80);

	struct ColTri
	{
		float         v[9];
		std::uint32_t flags;
	};
	static_assert(sizeof(ColTri) == 40);

	// ---- render ring (MC -> HL2) -----------------------------------------------------------
	inline constexpr std::uint64_t kRenRingHeadOff = 0x00;
	inline constexpr std::uint64_t kRenRingTailOff = 0x40;
	inline constexpr std::uint64_t kRenRingDataOff = 0x80;
	inline constexpr std::uint64_t kRenRingDataBytes = kRenderRingBytes - kRenRingDataOff;

	enum RenType : std::uint32_t
	{
		kRenPad         = 0,
		kRenAtlas       = 1, // RenAtlas + RGBA8 pixels (w * h * 4)
		kRenSection     = 2, // RenSection + RenVertex[vertexCount]
		kRenClearAll    = 3, // drop every section
		kRenTexture     = 4,
		kRenAvatar      = 5,
		kRenScene       = 6,
		kRenAtlasRegion = 7, // RenAtlasRegion + RGBA8 pixels
	};

	struct RenAtlas
	{
		std::uint32_t width, height;
	};

	struct RenAtlasRegion
	{
		std::uint32_t x, y, width, height;
	};

	struct RenSection
	{
		std::int32_t  sx, sy, sz;   // section coords (16-block cubes)
		std::uint32_t vertexCount;  // multiple of 3 (0 = remove section)
	};

	struct RenScene
	{
		double        originX, originY, originZ;
		std::uint32_t batchCount;
		std::uint32_t vertexCount;
	};
	static_assert(sizeof(RenScene) == 32);

	struct RenTexture
	{
		std::uint32_t id;
		std::uint32_t width, height;
		std::uint32_t pad;
	};
	static_assert(sizeof(RenTexture) == 16);

	struct RenBatch
	{
		std::uint32_t texture;
		std::uint32_t first;
		std::uint32_t count;
		std::uint32_t flags;
	};
	static_assert(sizeof(RenBatch) == 16);

	struct RenVertex
	{
		float         x, y, z;  // MC coords relative to the section origin (sx*16, sy*16, sz*16)
		float         u, v;     // atlas UV
		std::uint32_t color;    // RGBA8 (tint * AO)
		std::uint32_t light;    // low byte: block light 0-15, next byte: sky light 0-15
		std::uint32_t flags;    // bit0: cutout, bit1: translucent, bits 4-6: face normal
	};
	static_assert(sizeof(RenVertex) == 32);
}
