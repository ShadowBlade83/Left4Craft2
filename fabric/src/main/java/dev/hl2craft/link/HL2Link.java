package dev.hl2craft.link;

import static dev.hl2craft.link.Proto.*;
import static java.lang.foreign.ValueLayout.*;

import dev.hl2craft.HL2Craft;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.VarHandle;
import java.nio.charset.StandardCharsets;
import java.util.List;

public final class HL2Link {
	private static final int FILE_MAP_ALL_ACCESS = 0xF001F;
	private static final long HEARTBEAT_TIMEOUT_MS = 6000;

	private static final VarHandle INT = JAVA_INT.varHandle();
	private static final VarHandle LONG = JAVA_LONG.varHandle();

	private static final MethodHandle OPEN_FILE_MAPPING;
	private static final MethodHandle MAP_VIEW_OF_FILE;
	private static final MethodHandle GET_TICK_COUNT64;
	private static final MethodHandle GET_CURRENT_PROCESS_ID;

	static {
		Linker linker = Linker.nativeLinker();
		SymbolLookup k32 = SymbolLookup.libraryLookup("kernel32", Arena.global());
		OPEN_FILE_MAPPING = linker.downcallHandle(
			k32.find("OpenFileMappingW").orElseThrow(),
			FunctionDescriptor.of(ADDRESS, JAVA_INT, JAVA_INT, ADDRESS)
		);
		MAP_VIEW_OF_FILE = linker.downcallHandle(
			k32.find("MapViewOfFile").orElseThrow(),
			FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG)
		);
		GET_TICK_COUNT64 = linker.downcallHandle(
			k32.find("GetTickCount64").orElseThrow(),
			FunctionDescriptor.of(JAVA_LONG)
		);
		GET_CURRENT_PROCESS_ID = linker.downcallHandle(
			k32.find("GetCurrentProcessId").orElseThrow(),
			FunctionDescriptor.of(JAVA_INT)
		);
	}

	private static volatile MemorySegment shm;
	private static long lastOpenAttempt;
	private static int hostPid;
	private static volatile int generation;
	private static int overlayBack = 1;

	public record Actor(int entIndex, int flags, float x, float y, float z, float yaw, float width, float height, float hp, String name) {
		public boolean dead() {
			return (flags & ACTOR_DEAD) != 0;
		}
	}

	private static int badPoseLogs;

	public record HostSnapshot(int flags, int mapId, int epoch, double x, double y, double z, float yaw, float pitch, int teleportSeq, int width, int height, float fov, int gameMode) {
		public boolean inPlayable() {
			return (flags & HOST_IN_GAME) != 0 && (flags & HOST_MENU_OPEN) == 0 && (flags & HOST_LOADING) == 0;
		}

		public boolean onGround() {
			return (flags & HOST_ON_GROUND) != 0;
		}

		public boolean onLadder() {
			return (flags & HOST_ON_LADDER) != 0;
		}

		public boolean hl2PhysicsOverride() {
			return (flags & HOST_VEHICLE_OR_OVERRIDE) != 0;
		}
	}

	public record WorldEntity(int kind, int id, float x, float y, float z, float yaw, float pitch, float scale, float[] ext, float[] uv, int tint) {
	}

	private HL2Link() {
	}

	public static boolean active() {
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		long beat = (long) LONG.getAcquire(s, OFF_HEADER + H_HOST_HEARTBEAT);
		return tickCount() - beat < HEARTBEAT_TIMEOUT_MS;
	}

	public static int generation() {
		return generation;
	}

	public static MemorySegment segment() {
		return shm;
	}

	private static long tickCount() {
		try {
			return (long) GET_TICK_COUNT64.invokeExact();
		} catch (Throwable t) {
			return System.currentTimeMillis();
		}
	}

	public static void poll() {
		if (shm != null) {
			LONG.setRelease(shm, OFF_HEADER + H_MC_HEARTBEAT, tickCount());
			int pid = shm.get(JAVA_INT, OFF_HEADER + H_HOST_PID);
			if (pid != hostPid) {
				hostPid = pid;
				overlayBack = 1;
				generation++;
				HL2Craft.LOG.info("HL2Craft: HL2 instance changed (pid {})", pid);
			}
			return;
		}
		long now = System.currentTimeMillis();
		if (now - lastOpenAttempt < 1000) {
			return;
		}
		lastOpenAttempt = now;
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment name = arena.allocateFrom(MAPPING_NAME, StandardCharsets.UTF_16LE);
			MemorySegment handle = (MemorySegment) OPEN_FILE_MAPPING.invokeExact(FILE_MAP_ALL_ACCESS, 0, name);
			if (handle.address() == 0) {
				return;
			}
			MemorySegment view = (MemorySegment) MAP_VIEW_OF_FILE.invokeExact(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0L);
			if (view.address() == 0) {
				HL2Craft.LOG.error("HL2Craft: MapViewOfFile failed");
				return;
			}
			MemorySegment seg = view.reinterpret(MAPPING_BYTES);
			int magic = seg.get(JAVA_INT, OFF_HEADER + H_MAGIC);
			int version = seg.get(JAVA_INT, OFF_HEADER + H_VERSION);
			if (magic != MAGIC || version != VERSION) {
				HL2Craft.LOG.error("HL2Craft: protocol mismatch (magic {:x} ver {}); expected ver {}", magic, version, VERSION);
				return;
			}
			seg.set(JAVA_INT, OFF_HEADER + H_MC_PID, (int) GET_CURRENT_PROCESS_ID.invokeExact());
			LONG.setRelease(seg, OFF_HEADER + H_MC_HEARTBEAT, tickCount());
			hostPid = seg.get(JAVA_INT, OFF_HEADER + H_HOST_PID);
			generation++;
			shm = seg;
			HL2Craft.LOG.info("HL2Craft: successfully linked to HL2 (pid {})", hostPid);
		} catch (Throwable t) {
			HL2Craft.LOG.error("HL2Craft: failed to open shared memory", t);
		}
	}

	public static HostSnapshot readHost() {
		MemorySegment s = shm;
		if (s == null) {
			return null;
		}
		for (int attempt = 0; attempt < 5; attempt++) {
			int seq1 = (int) INT.getAcquire(s, OFF_HOST_STATE + HS_SEQ);
			if ((seq1 & 1) != 0) {
				continue;
			}
			int flags = s.get(JAVA_INT, OFF_HOST_STATE + HS_FLAGS);
			int mapId = s.get(JAVA_INT, OFF_HOST_STATE + HS_MAP_ID);
			int epoch = s.get(JAVA_INT, OFF_HOST_STATE + HS_COLLISION_EPOCH);
			double x = s.get(JAVA_DOUBLE, OFF_HOST_STATE + HS_POS_X);
			double y = s.get(JAVA_DOUBLE, OFF_HOST_STATE + HS_POS_Y);
			double z = s.get(JAVA_DOUBLE, OFF_HOST_STATE + HS_POS_Z);
			float yaw = s.get(JAVA_FLOAT, OFF_HOST_STATE + HS_YAW);
			float pitch = s.get(JAVA_FLOAT, OFF_HOST_STATE + HS_PITCH);
			int tel = s.get(JAVA_INT, OFF_HOST_STATE + HS_TELEPORT_SEQ);
			int w = s.get(JAVA_INT, OFF_HOST_STATE + HS_VIEWPORT_W);
			int h = s.get(JAVA_INT, OFF_HOST_STATE + HS_VIEWPORT_H);
			float fov = s.get(JAVA_FLOAT, OFF_HOST_STATE + HS_FOV);
			int mode = s.get(JAVA_INT, OFF_HOST_STATE + HS_GAMEMODE);
			int seq2 = (int) INT.getAcquire(s, OFF_HOST_STATE + HS_SEQ);
			if (seq1 == seq2) {
				// Left4Craft: a pose outside any map (non-finite, beyond the world border, or far outside build
				// height) is never applied: one such teleport killed the player at x = 5e281, y = NaN (2026-10-19)
				if (!Double.isFinite(x) || !Double.isFinite(y) || !Double.isFinite(z) || Math.abs(x) > 2.9e7 || Math.abs(z) > 2.9e7 || y < -2048.0 || y > 2048.0) {
					if (badPoseLogs++ < 20) {
						HL2Craft.LOG.warn("Left4Craft: ignoring impossible host pose {} {} {} (flags {}, teleport {})", x, y, z, flags, tel);
					}
					return null;
				}
				return new HostSnapshot(flags, mapId, epoch, x, y, z, yaw, pitch, tel, w, h, fov, mode);
			}
		}
		return null;
	}

	public static void writeMcState(int flags, double x, double y, double z, float yaw, float pitch, float eyeHeight, float sens, int telAck, int guiScale, long frame, float fov, int mode, int cameraMode, float cameraDistance, float supportHeight, float health) {
		writeMcState(flags, x, y, z, yaw, pitch, eyeHeight, sens, telAck, guiScale, frame, fov, mode, cameraMode, cameraDistance, supportHeight, health, 0, 0);
	}

	public static void writeMcState(int flags, double x, double y, double z, float yaw, float pitch, float eyeHeight, float sens, int telAck, int guiScale, long frame, float fov, int mode, int cameraMode, float cameraDistance, float supportHeight, float health, int heldLight, int heldLightType) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		int seq = (int) INT.getAcquire(s, OFF_MC_STATE + MS_SEQ);
		INT.setRelease(s, OFF_MC_STATE + MS_SEQ, seq + 1);
		s.set(JAVA_INT, OFF_MC_STATE + MS_FLAGS, flags);
		s.set(JAVA_DOUBLE, OFF_MC_STATE + MS_X, x);
		s.set(JAVA_DOUBLE, OFF_MC_STATE + MS_Y, y);
		s.set(JAVA_DOUBLE, OFF_MC_STATE + MS_Z, z);
		s.set(JAVA_FLOAT, OFF_MC_STATE + MS_YAW, yaw);
		s.set(JAVA_FLOAT, OFF_MC_STATE + MS_PITCH, pitch);
		s.set(JAVA_FLOAT, OFF_MC_STATE + MS_EYE_HEIGHT, eyeHeight);
		s.set(JAVA_FLOAT, OFF_MC_STATE + MS_SENSITIVITY, sens);
		s.set(JAVA_INT, OFF_MC_STATE + MS_TELEPORT_ACK, telAck);
		s.set(JAVA_INT, OFF_MC_STATE + MS_GUI_SCALE, guiScale);
		s.set(JAVA_LONG, OFF_MC_STATE + MS_FRAME_COUNTER, frame);
		s.set(JAVA_FLOAT, OFF_MC_STATE + MS_FOV_DEG, fov);
		s.set(JAVA_INT, OFF_MC_STATE + MS_GAMEMODE, mode);
		s.set(JAVA_INT, OFF_MC_STATE + MS_CAMERA_MODE, cameraMode);
		s.set(JAVA_FLOAT, OFF_MC_STATE + MS_CAMERA_DISTANCE, cameraDistance);
		s.set(JAVA_FLOAT, OFF_MC_STATE + MS_SUPPORT_HEIGHT, supportHeight);
		s.set(JAVA_FLOAT, OFF_MC_STATE + MS_HEALTH, health);
		s.set(JAVA_INT, OFF_MC_STATE + MS_HELD_LIGHT, heldLight);
		s.set(JAVA_INT, OFF_MC_STATE + MS_HELD_LIGHT_TYPE, heldLightType);
		INT.setRelease(s, OFF_MC_STATE + MS_SEQ, seq + 2);
	}

	public interface InputConsumer {
		void accept(int type, int code, int a, int b, int c);
	}

	public static void drainInput(InputConsumer consumer) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long head = (long) LONG.getAcquire(s, OFF_INPUT_RING + IR_HEAD);
		long tail = (long) LONG.getAcquire(s, OFF_INPUT_RING + IR_TAIL);
		if (tail >= head) {
			return;
		}
		while (tail < head) {
			long off = OFF_INPUT_RING + IR_DATA + (tail % INPUT_RING_ENTRIES) * INPUT_EVENT_BYTES;
			short type = s.get(JAVA_SHORT, off);
			short code = s.get(JAVA_SHORT, off + 2);
			int a = s.get(JAVA_INT, off + 4);
			int b = s.get(JAVA_INT, off + 8);
			int c = s.get(JAVA_INT, off + 12);
			consumer.accept(type, code, a, b, c);
			tail++;
		}
		LONG.setRelease(s, OFF_INPUT_RING + IR_TAIL, tail);
	}

	public static void pushEvent(int type, int entIndex, float a, float b, float c, float d, int flags, int weapon) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long head = (long) LONG.getAcquire(s, OFF_EVENT_RING + ER_HEAD);
		long tail = (long) LONG.getAcquire(s, OFF_EVENT_RING + ER_TAIL);
		if (head - tail >= EVENT_RING_ENTRIES) {
			return;
		}
		long off = OFF_EVENT_RING + ER_DATA + (head % EVENT_RING_ENTRIES) * EVENT_BYTES;
		s.set(JAVA_INT, off, type);
		s.set(JAVA_INT, off + 4, entIndex);
		s.set(JAVA_FLOAT, off + 8, a);
		s.set(JAVA_FLOAT, off + 12, b);
		s.set(JAVA_FLOAT, off + 16, c);
		s.set(JAVA_FLOAT, off + 20, d);
		s.set(JAVA_INT, off + 24, flags);
		s.set(JAVA_INT, off + 28, weapon);
		LONG.setRelease(s, OFF_EVENT_RING + ER_HEAD, head + 1);
	}

	public static boolean readActors(List<Actor> out) {
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		out.clear();
		for (int attempt = 0; attempt < 5; attempt++) {
			int seq1 = (int) INT.getAcquire(s, OFF_ACTOR_TABLE + AT_SEQ);
			if ((seq1 & 1) != 0) {
				continue;
			}
			int count = Math.min(s.get(JAVA_INT, OFF_ACTOR_TABLE + AT_COUNT), MAX_ACTORS);
			long off = OFF_ACTOR_TABLE + AT_RECORDS;
			for (int i = 0; i < count; i++, off += ACTOR_RECORD_BYTES) {
				int ent = s.get(JAVA_INT, off);
				int flags = s.get(JAVA_INT, off + 4);
				float x = s.get(JAVA_FLOAT, off + 8);
				float y = s.get(JAVA_FLOAT, off + 12);
				float z = s.get(JAVA_FLOAT, off + 16);
				float yaw = s.get(JAVA_FLOAT, off + 20);
				float w = s.get(JAVA_FLOAT, off + 24);
				float h = s.get(JAVA_FLOAT, off + 28);
				float hp = s.get(JAVA_FLOAT, off + 32);
				byte[] nameBytes = new byte[24];
				MemorySegment.copy(s, off + 40, MemorySegment.ofArray(nameBytes), 0, 24);
				int len = 0;
				while (len < 24 && nameBytes[len] != 0) len++;
				String name = new String(nameBytes, 0, len, StandardCharsets.UTF_8);
				out.add(new Actor(ent, flags, x, y, z, yaw, w, h, hp, name));
			}
			int seq2 = (int) INT.getAcquire(s, OFF_ACTOR_TABLE + AT_SEQ);
			if (seq1 == seq2) {
				return true;
			}
			out.clear();
		}
		return false;
	}

	/** Seqlock write of the world-entity table and block selection. Render thread only. */
	public static void writeWorldEntities(java.util.List<WorldEntity> entities, float[] selection) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long b = OFF_WORLD_ENTITIES;
		int seq = s.get(JAVA_INT, b + WE_SEQ);
		INT.setRelease(s, b + WE_SEQ, seq + 1);
		VarHandle.storeStoreFence();
		int count = Math.min(entities.size(), MAX_WORLD_ENTITIES);
		s.set(JAVA_INT, b + WE_COUNT, count);
		s.set(JAVA_INT, b + WE_HAS_SELECTION, selection != null ? 1 : 0);
		if (selection != null) {
			for (int i = 0; i < 6; i++) {
				s.set(JAVA_FLOAT, b + WE_SEL_MIN + i * 4L, selection[i]);
			}
		}
		for (int i = 0; i < count; i++) {
			WorldEntity w = entities.get(i);
			long r = b + WE_RECORDS + i * WORLD_ENTITY_BYTES;
			s.set(JAVA_INT, r, w.kind());
			s.set(JAVA_INT, r + 4, w.id());
			s.set(JAVA_FLOAT, r + 8, w.x());
			s.set(JAVA_FLOAT, r + 12, w.y());
			s.set(JAVA_FLOAT, r + 16, w.z());
			s.set(JAVA_FLOAT, r + 20, w.yaw());
			s.set(JAVA_FLOAT, r + 24, w.pitch());
			s.set(JAVA_FLOAT, r + 28, w.scale());
			for (int k = 0; k < 3; k++) {
				s.set(JAVA_FLOAT, r + 32 + k * 4L, w.ext() != null ? w.ext()[k] : 0.0F);
			}
			for (int k = 0; k < 12; k++) {
				s.set(JAVA_FLOAT, r + 44 + k * 4L, w.uv() != null && k < w.uv().length ? w.uv()[k] : 0.0F);
			}
			s.set(JAVA_INT, r + 92, w.tint());
		}
		INT.setRelease(s, b + WE_SEQ, seq + 2);
	}

	public static long collisionHead() {
		MemorySegment s = shm;
		return s == null ? 0 : (long) LONG.getAcquire(s, OFF_COLLISION_RING + CR_HEAD);
	}

	public static long collisionTail() {
		MemorySegment s = shm;
		return s == null ? 0 : (long) LONG.getAcquire(s, OFF_COLLISION_RING + CR_TAIL);
	}

	public static void setCollisionTail(long tail) {
		MemorySegment s = shm;
		if (s != null) {
			LONG.setRelease(s, OFF_COLLISION_RING + CR_TAIL, tail);
		}
	}

	public static long overlayBackSlotOffset() {
		return OFF_OVERLAY_PIXELS + (long) overlayBack * OVERLAY_SLOT_BYTES;
	}

	public static void publishOverlay(int w, int h, boolean bottomUp, long frameId) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long hdrOff = OFF_OVERLAY_SLOT_HDR + (long) overlayBack * 0x40L;
		s.set(JAVA_INT, hdrOff, w);
		s.set(JAVA_INT, hdrOff + 4, h);
		s.set(JAVA_INT, hdrOff + 8, bottomUp ? 1 : 0);
		s.set(JAVA_LONG, hdrOff + 16, frameId);

		int state = (int) INT.getAcquire(s, OFF_OVERLAY_CTL);
		while (true) {
			int mid = state & 3;
			int nextState = overlayBack | (1 << 2);
			int prev = (int) INT.compareAndExchangeRelease(s, OFF_OVERLAY_CTL, state, nextState);
			if (prev == state) {
				overlayBack = mid;
				break;
			}
			state = prev;
		}
	}

	public static boolean writeRender(int type, java.nio.ByteBuffer header, java.nio.ByteBuffer body) {
		return writeRender(type, header, body, 5);
	}

	public static boolean tryWriteRender(int type, java.nio.ByteBuffer header, java.nio.ByteBuffer body) {
		return writeRender(type, header, body, 1);
	}

	private static synchronized boolean writeRender(int type, java.nio.ByteBuffer header, java.nio.ByteBuffer body, int attempts) {
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		int payload = header.remaining() + (body != null ? body.remaining() : 0);
		long msgBytes = (8 + payload + 7) & ~7L;
		if (msgBytes > RR_DATA_BYTES - 65536L) {
			HL2Craft.LOG.warn("HL2Craft: render message too large ({} bytes)", msgBytes);
			return false;
		}
		long base = OFF_RENDER_RING;
		for (int attempt = 0; attempt < attempts; attempt++) {
			long head = s.get(JAVA_LONG, base + RR_HEAD);
			long tail = (long) LONG.getAcquire(s, base + RR_TAIL);
			long pos = head % RR_DATA_BYTES;
			long pad = pos + msgBytes > RR_DATA_BYTES ? RR_DATA_BYTES - pos : 0;
			if (RR_DATA_BYTES - (head - tail) < msgBytes + pad) {
				if (attempt + 1 >= attempts) {
					break;
				}
				try {
					Thread.sleep(2);
				} catch (InterruptedException e) {
					return false;
				}
				continue;
			}
			if (pad > 0) {
				s.set(JAVA_INT, base + RR_DATA + pos, REN_PAD);
				s.set(JAVA_INT, base + RR_DATA + pos + 4, 0);
				head += pad;
				pos = 0;
			}
			long at = base + RR_DATA + pos;
			s.set(JAVA_INT, at, type);
			s.set(JAVA_INT, at + 4, payload);
			MemorySegment.copy(MemorySegment.ofBuffer(header), 0, s, at + 8, header.remaining());
			if (body != null && body.remaining() > 0) {
				MemorySegment.copy(MemorySegment.ofBuffer(body), 0, s, at + 8 + header.remaining(), body.remaining());
			}
			LONG.setRelease(s, base + RR_HEAD, head + msgBytes);
			return true;
		}
		return false;
	}
}
