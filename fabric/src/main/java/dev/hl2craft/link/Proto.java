package dev.hl2craft.link;

/**
 * Mirror of protocol/hl2craft_protocol.h. Keep byte layout in sync.
 */
public final class Proto {
	private Proto() {
	}

	public static final int MAGIC = 0x43324C48; // "HL2C"
	public static final int VERSION = 14;
	public static final String MAPPING_NAME = System.getProperty("hl2craft.link", "Local\\HL2Minecraft_v1");

	public static final double UNITS_PER_BLOCK = 39.37007874015748;
	public static final double METERS_PER_UNIT = 0.0254;

	// Offsets
	public static final long OFF_HEADER = 0x0;
	public static final long OFF_HOST_STATE = 0x100;
	public static final long OFF_MC_STATE = 0x200;
	public static final long OFF_OVERLAY_CTL = 0x300;
	public static final long OFF_OVERLAY_SLOT_HDR = 0x340;
	public static final long OFF_INPUT_RING = 0x1000;
	public static final long OFF_ACTOR_TABLE = 0x12000;
	public static final long OFF_EVENT_RING = 0x17000;
	public static final long OFF_WORLD_ENTITIES = 0x1C000;
	public static final long OFF_COLLISION_RING = 0x20000;
	public static final long COLLISION_RING_BYTES = 32L << 20; // 32MB
	public static final long OFF_OVERLAY_PIXELS = OFF_COLLISION_RING + COLLISION_RING_BYTES;
	public static final int MAX_OVERLAY_W = 3840;
	public static final int MAX_OVERLAY_H = 2160;
	public static final long OVERLAY_SLOT_BYTES = (long) MAX_OVERLAY_W * MAX_OVERLAY_H * 4L;
	public static final int OVERLAY_SLOTS = 3;
	public static final long OFF_RENDER_RING = OFF_OVERLAY_PIXELS + OVERLAY_SLOT_BYTES * OVERLAY_SLOTS;
	public static final long RENDER_RING_BYTES = 128L << 20; // 128MB
	public static final long OFF_FRAME_BUFFER = OFF_RENDER_RING;
	public static final long FRAME_BUFFER_BYTES = RENDER_RING_BYTES;
	public static final long MAPPING_BYTES = OFF_RENDER_RING + RENDER_RING_BYTES;

	// Header fields
	public static final long H_MAGIC = 0x00;
	public static final long H_VERSION = 0x04;
	public static final long H_HOST_PID = 0x08;
	public static final long H_MC_PID = 0x0C;
	public static final long H_HOST_HEARTBEAT = 0x10;
	public static final long H_MC_HEARTBEAT = 0x18;

	// Host state (HL2 -> MC)
	public static final long HS_SEQ = 0x00;
	public static final long HS_FLAGS = 0x04;
	public static final long HS_MAP_ID = 0x08;
	public static final long HS_COLLISION_EPOCH = 0x0C;
	public static final long HS_POS_X = 0x10;
	public static final long HS_POS_Y = 0x18;
	public static final long HS_POS_Z = 0x20;
	public static final long HS_YAW = 0x28;
	public static final long HS_PITCH = 0x2C;
	public static final long HS_TELEPORT_SEQ = 0x30;
	public static final long HS_VIEWPORT_W = 0x34;
	public static final long HS_VIEWPORT_H = 0x38;
	public static final long HS_FOV = 0x3C;
	public static final long HS_GAMEMODE = 0x40;

	public static final int HOST_IN_GAME = 1;
	public static final int HOST_MENU_OPEN = 2;
	public static final int HOST_LOADING = 4;
	public static final int HOST_PAUSED = 8;
	public static final int HOST_ON_GROUND = 16;
	public static final int HOST_ON_LADDER = 32;
	public static final int HOST_VEHICLE_OR_OVERRIDE = 64;

	// MC state (MC -> HL2)
	public static final long MS_SEQ = 0x00;
	public static final long MS_FLAGS = 0x04;
	public static final long MS_X = 0x08;
	public static final long MS_Y = 0x10;
	public static final long MS_Z = 0x18;
	public static final long MS_YAW = 0x20;
	public static final long MS_PITCH = 0x24;
	public static final long MS_EYE_HEIGHT = 0x28;
	public static final long MS_SENSITIVITY = 0x2C;
	public static final long MS_TELEPORT_ACK = 0x30;
	public static final long MS_GUI_SCALE = 0x34;
	public static final long MS_FRAME_COUNTER = 0x38;
	public static final long MS_FOV_DEG = 0x40;
	public static final long MS_GAMEMODE = 0x44;
	public static final long MS_CAMERA_MODE = 0x48;
	public static final long MS_CAMERA_DISTANCE = 0x4C;
	public static final long MS_SUPPORT_HEIGHT = 0x50;
	public static final long MS_HEALTH = 0x54;
	public static final long MS_HELD_LIGHT = 0x58;
	public static final long MS_HELD_LIGHT_TYPE = 0x5C;

	public static final int MC_IN_WORLD = 1;
	public static final int MC_SCREEN_OPEN = 2;
	public static final int MC_ON_GROUND = 4;
	public static final int MC_SNEAKING = 8;
	public static final int MC_SPRINTING = 16;
	public static final int MC_DEAD = 32;
	public static final int MC_SWIMMING = 64;
	public static final int MC_FLYING = 128;
	public static final int MC_BLOCKING = 256;

	// Input ring
	public static final int INPUT_RING_ENTRIES = 4096;
	public static final long IR_HEAD = 0x00;
	public static final long IR_TAIL = 0x40;
	public static final long IR_DATA = 0x80;
	public static final long INPUT_EVENT_BYTES = 16;

	public static final int IN_KEY = 1;
	public static final int IN_MOUSE_BUTTON = 2;
	public static final int IN_SCROLL = 3;
	public static final int IN_CURSOR = 4;
	public static final int IN_TEXT = 5;
	public static final int IN_RELEASE_ALL = 6;
	public static final int IN_HURT = 7;
	public static final int IN_OPEN_MENU = 8;
	/** Left4Craft: the infected broke a barricade block in L4D2; a, b, c = block x, y, z. */
	public static final int IN_BREAK_BLOCK = 100;

	// Actor table
	public static final int MAX_ACTORS = 256;
	public static final long AT_SEQ = 0x00;
	public static final long AT_COUNT = 0x04;
	public static final long AT_RECORDS = 0x40;
	public static final long ACTOR_RECORD_BYTES = 64;

	public static final int ACTOR_HOSTILE = 1;
	public static final int ACTOR_DEAD = 2;
	public static final int ACTOR_FRIEND = 4;
	public static final int ACTOR_IN_COMBAT = 8;
	public static final int ACTOR_ARMED = 16;

	// Event ring
	public static final int EVENT_RING_ENTRIES = 512;
	public static final long ER_HEAD = 0x00;
	public static final long ER_TAIL = 0x40;
	public static final long ER_DATA = 0x80;
	public static final long EVENT_BYTES = 32;

	public static final int EV_HIT_ACTOR = 1;
	public static final int EV_PLAYER_DIED = 2;
	public static final int EV_EXPLOSION = 3;
	public static final int EV_TELEPORT = 4;
	public static final int EV_BLOCK_PLACED = 5;
	public static final int EV_BLOCK_BROKEN = 6;
	public static final int EV_CLEAR_BLOCKS = 7;

	public static final int WEAPON_UNARMED = 0;
	public static final int WEAPON_BLADE = 1;
	public static final int WEAPON_AXE = 2;
	public static final int WEAPON_PICKAXE = 3;
	public static final int WEAPON_ARROW = 4;

	public static final int HIT_CRIT = 1;
	public static final int HIT_PROJECTILE = 2;
	public static final int HIT_ON_FIRE = 4;

	// World entities @0x1C000
	public static final int MAX_WORLD_ENTITIES = 160;
	public static final long WE_SEQ = 0x00;
	public static final long WE_COUNT = 0x04;
	public static final long WE_HAS_SELECTION = 0x08;
	public static final long WE_SEL_MIN = 0x0C;
	public static final long WE_SEL_MAX = 0x18;
	public static final long WE_RECORDS = 0x40;
	public static final long WORLD_ENTITY_BYTES = 96;

	public static final int WE_ARROW = 1;
	public static final int WE_ITEM = 2;
	public static final int WE_TRIDENT = 3;
	public static final int WE_BLOCK = 4;
	public static final int WE_CRACK = 5;
	public static final int WE_SHADOW = 6;
	public static final int WE_MOB = 7;

	// Collision ring
	public static final long CR_HEAD = 0x00;
	public static final long CR_TAIL = 0x40;
	public static final long CR_DATA = 0x80;
	public static final long CR_DATA_BYTES = COLLISION_RING_BYTES - CR_DATA;

	public static final int COL_PAD = 0;
	public static final int COL_CLEAR = 1;
	public static final int COL_REGION = 2;
	public static final int COL_TRIS = 3;

	public static final long COL_REGION_HEADER_BYTES = 32;
	public static final long COL_BLOCK_BYTES = 80;
	public static final long COL_TRI_BYTES = 40;

	// Render ring (MC -> HL2)
	public static final long RR_HEAD = 0x00;
	public static final long RR_TAIL = 0x40;
	public static final long RR_DATA = 0x80;
	public static final long RR_DATA_BYTES = RENDER_RING_BYTES - RR_DATA;

	public static final int REN_PAD = 0;
	public static final int REN_ATLAS = 1;
	public static final int REN_SECTION = 2;
	public static final int REN_CLEAR_ALL = 3;
	public static final int REN_TEXTURE = 4;
	public static final int REN_AVATAR = 5;
	public static final int REN_SCENE = 6;
	public static final int REN_ATLAS_REGION = 7;

	public static final int REN_VERTEX_BYTES = 32;
	public static final int REN_SCENE_BYTES = 32;
	public static final int REN_TEXTURE_BYTES = 16;
	public static final int REN_BATCH_BYTES = 16;
}
