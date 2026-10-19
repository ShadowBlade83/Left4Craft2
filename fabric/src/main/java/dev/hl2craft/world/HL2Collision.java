package dev.hl2craft.world;

import static dev.hl2craft.link.Proto.*;
import static java.lang.foreign.ValueLayout.*;

import dev.hl2craft.HL2Craft;
import dev.hl2craft.link.HL2Link;
import java.lang.foreign.MemorySegment;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.world.phys.shapes.BitSetDiscreteVoxelShape;
import net.minecraft.world.phys.shapes.CubeVoxelShape;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.jspecify.annotations.Nullable;

public final class HL2Collision {
	public static final int REGION_SIZE = 8;

	private static final ConcurrentHashMap<Long, VoxelShape> SHAPES = new ConcurrentHashMap<>();
	private static volatile int epoch = -1;
	private static Thread consumer;

	private HL2Collision() {
	}

	/**
	 * Left4Craft debug (F8): L4D2's collision around Steve's feet as text in the log, one row of cells per z, for
	 * y from feet-1 to feet+2: '#' full block, '.' empty, a digit 1-8 = a partial cell whose top is that many eighths up.
	 */
	public static void dumpAround(BlockPos feet, net.minecraft.world.phys.Vec3 exact) {
		StringBuilder b = new StringBuilder(String.format(java.util.Locale.ROOT,
			"Left4Craft collision around %.3f %.3f %.3f (epoch %d, %d cells):", exact.x, exact.y, exact.z, epoch, SHAPES.size()));
		for (int dy = 2; dy >= -1; dy--) {
			b.append("\n  y=").append(feet.getY() + dy).append(dy == 0 ? " (feet)" : "");
			for (int dz = -2; dz <= 2; dz++) {
				b.append("\n    ");
				for (int dx = -2; dx <= 2; dx++) {
					VoxelShape v = SHAPES.get(BlockPos.asLong(feet.getX() + dx, feet.getY() + dy, feet.getZ() + dz));
					char c;
					if (v == null || v.isEmpty()) {
						c = '.';
					} else if (v == Shapes.block()) {
						c = '#';
					} else {
						c = (char) ('0' + Math.max(1, Math.min(8, (int) Math.round(v.max(net.minecraft.core.Direction.Axis.Y) * 8))));
					}
					b.append(dx == 0 && dz == 0 ? '[' : ' ').append(c).append(dx == 0 && dz == 0 ? ']' : ' ');
				}
			}
		}
		dev.hl2craft.HL2Craft.LOG.info(b.toString());
	}

	public static @Nullable VoxelShape shapeAt(BlockPos pos) {
		return SHAPES.isEmpty() ? null : SHAPES.get(pos.asLong());
	}

	public static boolean hasGeometry(BlockPos pos) {
		return SHAPES.containsKey(pos.asLong());
	}

	public static int blockCount() {
		return SHAPES.size();
	}

	public static synchronized void startConsumer() {
		if (consumer != null) {
			return;
		}
		consumer = new Thread(HL2Collision::consumeLoop, "HL2Craft collision");
		consumer.setDaemon(true);
		consumer.start();
	}

	private static void consumeLoop() {
		while (true) {
			try {
				if (!drainOnce()) {
					Thread.sleep(2);
				}
			} catch (InterruptedException e) {
				return;
			} catch (Throwable t) {
				HL2Craft.LOG.error("HL2Craft: collision consumer error", t);
				try {
					Thread.sleep(200);
				} catch (InterruptedException e) {
					return;
				}
			}
		}
	}

	private static boolean drainOnce() {
		MemorySegment s = HL2Link.segment();
		if (s == null) {
			return false;
		}
		long head = HL2Link.collisionHead();
		long tail = HL2Link.collisionTail();
		if (tail >= head) {
			return false;
		}
		long data = OFF_COLLISION_RING + CR_DATA;
		while (tail < head) {
			long pos = tail % CR_DATA_BYTES;
			int type = s.get(JAVA_INT, data + pos);
			int payloadBytes = s.get(JAVA_INT, data + pos + 4);
			if (type == COL_PAD) {
				tail += CR_DATA_BYTES - pos;
				continue;
			}
			long payload = data + pos + 8;
			switch (type) {
				case COL_CLEAR -> clear(s.get(JAVA_INT, payload));
				case COL_REGION -> readRegion(s, payload);
				default -> {}
			}
			tail += (8 + payloadBytes + 7) & ~7L;
		}
		HL2Link.setCollisionTail(tail);
		return true;
	}

	private static final java.util.Set<Long> BARRIER_BLOCKS = java.util.Collections.newSetFromMap(new ConcurrentHashMap<>());
	private static final java.util.Set<BlockPos> PLACED_BLOCKS = java.util.Collections.newSetFromMap(new ConcurrentHashMap<>());
	private static volatile boolean pendingClear = false;

	public static void trackPlacedBlock(BlockPos pos) {
		PLACED_BLOCKS.add(pos.immutable());
	}

	public static boolean untrackPlacedBlock(BlockPos pos) {
		return PLACED_BLOCKS.remove(pos);
	}

	public static boolean isPlacedBlock(BlockPos pos) {
		return PLACED_BLOCKS.contains(pos);
	}

	private static final ConcurrentHashMap<Long, Integer> COLUMN_FLOOR_Y = new ConcurrentHashMap<>();
	private static final ConcurrentHashMap<Long, Integer> CHUNK_FLOOR_Y = new ConcurrentHashMap<>();

	private static long colKey(int x, int z) {
		return ((long) x << 32) | (z & 0xFFFFFFFFL);
	}

	private static void clear(int newEpoch) {
		SHAPES.clear();
		COLUMN_FLOOR_Y.clear();
		CHUNK_FLOOR_Y.clear();
		epoch = newEpoch;
		pendingClear = true;
		HL2Craft.LOG.info("HL2Craft: collision cleared (epoch {})", newEpoch);
	}

	public static void clearAllEntities(net.minecraft.server.level.ServerLevel level) {
		if (level == null) return;
		try {
			java.util.List<net.minecraft.world.entity.Entity> toDiscard = new java.util.ArrayList<>();
			for (net.minecraft.world.entity.Entity e : level.getAllEntities()) {
				if (e != null && !(e instanceof net.minecraft.world.entity.player.Player)) {
					toDiscard.add(e);
				}
			}
			for (net.minecraft.world.entity.Entity e : toDiscard) {
				e.discard();
			}
		} catch (Exception e) {
			HL2Craft.LOG.warn("HL2Craft: Error clearing entities: {}", e.getMessage());
		}
	}

	public static void clearAllPlacedBlocks(net.minecraft.server.level.ServerLevel level) {
		if (!PLACED_BLOCKS.isEmpty()) {
			for (BlockPos bp : PLACED_BLOCKS) {
				if (level != null && bp != null) {
					if (!level.isLoaded(bp)) {
						level.getChunk(bp.getX() >> 4, bp.getZ() >> 4);
					}
					if (!level.getBlockState(bp).isAir()) {
						level.setBlock(bp, net.minecraft.world.level.block.Blocks.AIR.defaultBlockState(), 3);
					}
				}
			}
			PLACED_BLOCKS.clear();
		}

		if (level != null) {
			try {
				var players = level.players();
				if (!players.isEmpty()) {
					var p = players.getFirst();
					int pcx = p.chunkPosition().x();
					int pcz = p.chunkPosition().z();
					for (int cx = pcx - 10; cx <= pcx + 10; cx++) {
						for (int cz = pcz - 10; cz <= pcz + 10; cz++) {
							var chunk = level.getChunkSource().getChunkNow(cx, cz);
							if (chunk != null) {
								var sections = chunk.getSections();
								for (int secIdx = 0; secIdx < sections.length; secIdx++) {
									var sec = sections[secIdx];
									if (sec != null && !sec.hasOnlyAir()) {
										int secBottomY = chunk.getSectionYFromSectionIndex(secIdx) << 4;
										for (int bx = 0; bx < 16; bx++) {
											for (int bz = 0; bz < 16; bz++) {
												for (int by = 0; by < 16; by++) {
													var st = sec.getBlockState(bx, by, bz);
													if (!st.isAir()) {
														BlockPos wp = new BlockPos((cx << 4) + bx, secBottomY + by, (cz << 4) + bz);
														level.setBlock(wp, net.minecraft.world.level.block.Blocks.AIR.defaultBlockState(), 3);
													}
												}
											}
										}
									}
								}
							}
						}
					}
				}
			} catch (Exception ex) {
				HL2Craft.LOG.warn("HL2Craft: Error scanning chunk sections: {}", ex.getMessage());
			}
		}

		HL2Link.pushEvent(dev.hl2craft.link.Proto.EV_CLEAR_BLOCKS, 0, 0, 0, 0, 0, 0, 0);
		clearAllEntities(level);
	}

	public static void serverTick(net.minecraft.server.MinecraftServer server) {
		if (server == null || !HL2Link.active()) {
			return;
		}
		var players = server.getPlayerList().getPlayers();
		if (players.isEmpty()) {
			return;
		}
		var player = players.getFirst();
		net.minecraft.server.level.ServerLevel level = player.level();
		if (level == null) {
			return;
		}

		boolean playerDead = player.isDeadOrDying();
		if (pendingClear || playerDead) {
			clearAllPlacedBlocks(level);
		}

		if (pendingClear || !BARRIER_BLOCKS.isEmpty()) {
			pendingClear = false;
			for (long packed : BARRIER_BLOCKS) {
				BlockPos bp = BlockPos.of(packed);
				if (level.isLoaded(bp) && level.getBlockState(bp).is(net.minecraft.world.level.block.Blocks.BARRIER)) {
					level.setBlock(bp, net.minecraft.world.level.block.Blocks.AIR.defaultBlockState(), 2);
				}
			}
			BARRIER_BLOCKS.clear();
		}

		// Ensure chunks and floor collision remain loaded under all active entities/mobs even when far from player
		try {
			int defaultFloorY = (int) Math.floor(player.getY()) - 1;
			for (net.minecraft.world.entity.Entity ent : level.getAllEntities()) {
				if (ent == null || ent instanceof net.minecraft.world.entity.player.Player || ent.isRemoved()) {
					continue;
				}
				int ex = ent.getBlockX();
				int ey = ent.getBlockY();
				int ez = ent.getBlockZ();
				int ecx = ex >> 4;
				int ecz = ez >> 4;
				if (!level.hasChunk(ecx, ecz)) {
					level.getChunk(ecx, ecz);
				}
				long ck = colKey(ecx, ecz);
				int chunkFloor = CHUNK_FLOOR_Y.getOrDefault(ck, Math.max(ey - 1, defaultFloorY));
				for (int dx = -1; dx <= 1; dx++) {
					for (int dz = -1; dz <= 1; dz++) {
						int bx = ex + dx;
						int bz = ez + dz;
						long col = colKey(bx, bz);
						if (!COLUMN_FLOOR_Y.containsKey(col)) {
							int fy = Math.min(ey - 1, chunkFloor);
							COLUMN_FLOOR_Y.put(col, fy);
							SHAPES.putIfAbsent(BlockPos.asLong(bx, fy, bz), Shapes.block());
							SHAPES.putIfAbsent(BlockPos.asLong(bx, fy - 1, bz), Shapes.block());
						}
					}
				}
			}
		} catch (Exception ignored) {
		}

		// Sweep any legacy barrier blocks saved on disk in chunks near the player
		if ((server.getTickCount() & 19) == 0) {
			int pcx = player.chunkPosition().x();
			int pcz = player.chunkPosition().z();
			for (int cx = pcx - 2; cx <= pcx + 2; cx++) {
				for (int cz = pcz - 2; cz <= pcz + 2; cz++) {
					var chunk = level.getChunkSource().getChunkNow(cx, cz);
					if (chunk != null) {
						var sections = chunk.getSections();
						for (int secIdx = 0; secIdx < sections.length; secIdx++) {
							var sec = sections[secIdx];
							if (sec != null && !sec.hasOnlyAir()) {
								int secBottomY = chunk.getSectionYFromSectionIndex(secIdx) << 4;
								for (int bx = 0; bx < 16; bx++) {
									for (int bz = 0; bz < 16; bz++) {
										for (int by = 0; by < 16; by++) {
											if (sec.getBlockState(bx, by, bz).is(net.minecraft.world.level.block.Blocks.BARRIER)) {
												BlockPos wp = new BlockPos((cx << 4) + bx, secBottomY + by, (cz << 4) + bz);
												level.setBlock(wp, net.minecraft.world.level.block.Blocks.AIR.defaultBlockState(), 2);
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}

	public static double ensurePlayerFloorAndStep(double px, double py, double pz, double fallbackFloorY) {
		int bx = (int) Math.floor(px);
		int bz = (int) Math.floor(pz);

		// Only create temporary bootstrap floor before the very first HL2 ScanCollision arrives
		if (SHAPES.isEmpty()) {
			int fallbackBy = (int) Math.floor(fallbackFloorY - 0.15);
			for (int dx = -1; dx <= 1; dx++) {
				for (int dz = -1; dz <= 1; dz++) {
					int cx = bx + dx;
					int cz = bz + dz;
					SHAPES.putIfAbsent(BlockPos.asLong(cx, fallbackBy, cz), Shapes.block());
					SHAPES.putIfAbsent(BlockPos.asLong(cx, fallbackBy - 1, cz), Shapes.block());
				}
			}
		}

		Integer fy = COLUMN_FLOOR_Y.get(colKey(bx, bz));
		if (fy != null) {
			VoxelShape s = SHAPES.get(BlockPos.asLong(bx, fy, bz));
			if (s != null && !s.isEmpty()) {
				return fy + s.max(net.minecraft.core.Direction.Axis.Y);
			}
		}
		return Double.NaN;
	}

	private static void readRegion(MemorySegment s, long p) {
		int minX = s.get(JAVA_INT, p);
		int minY = s.get(JAVA_INT, p + 4);
		int minZ = s.get(JAVA_INT, p + 8);
		int maxX = s.get(JAVA_INT, p + 12);
		int maxY = s.get(JAVA_INT, p + 16);
		int maxZ = s.get(JAVA_INT, p + 20);
		int msgEpoch = s.get(JAVA_INT, p + 24);
		int count = s.get(JAVA_INT, p + 28);

		if (epoch == -1) {
			epoch = msgEpoch;
		} else if (msgEpoch != epoch) {
			return; // Stale data from previous level
		}

		// Left4Craft: the scan can send one cell twice (a slope's upper block and a wall at the same y): OR the masks
		// together. HL2Craft kept the last one, which dropped floors or added walls on stairs and slopes.
		java.util.LinkedHashMap<Long, long[]> merged = new java.util.LinkedHashMap<>(count * 2);
		long e = p + COL_REGION_HEADER_BYTES;
		for (int i = 0; i < count; i++, e += COL_BLOCK_BYTES) {
			int x = s.get(JAVA_INT, e);
			int y = s.get(JAVA_INT, e + 4);
			int z = s.get(JAVA_INT, e + 8);
			long[] layers = merged.computeIfAbsent(BlockPos.asLong(x, y, z), k -> new long[8]);
			for (int l = 0; l < 8; l++) {
				layers[l] |= s.get(JAVA_LONG, e + 16 + l * 8L);
			}
		}

		java.util.HashMap<Long, VoxelShape> next = new java.util.HashMap<>(merged.size() * 2);
		java.util.HashSet<Long> seenFloorCols = new java.util.HashSet<>(merged.size());
		for (var entry : merged.entrySet()) {
			VoxelShape shape = buildShape(entry.getValue());
			if (shape == null) {
				continue;
			}
			long packed = entry.getKey();
			next.put(packed, shape);
			int x = BlockPos.getX(packed), y = BlockPos.getY(packed), z = BlockPos.getZ(packed);
			long col = colKey(x, z);
			if (seenFloorCols.add(col)) {
				COLUMN_FLOOR_Y.put(col, y);
				CHUNK_FLOOR_Y.put(colKey(x >> 4, z >> 4), y);
			}
		}

		// The whole scanned box is replaced. HL2Craft kept a 1-block rim, where walls from scans made from other
		// positions stayed behind (invisible walls on stairs). An empty region now clears its box too.
		SHAPES.keySet().removeIf(packed -> {
			int bx = BlockPos.getX(packed);
			int by = BlockPos.getY(packed);
			int bz = BlockPos.getZ(packed);
			return bx >= minX && bx <= maxX
				&& by >= minY && by <= maxY
				&& bz >= minZ && bz <= maxZ
				&& !next.containsKey(packed);
		});
		SHAPES.putAll(next);
	}

	private static @Nullable VoxelShape buildShape(long[] layers) {
		boolean any = false;
		boolean full = true;
		for (int y = 0; y < 8; y++) {
			any |= layers[y] != 0;
			full &= layers[y] == -1L;
		}
		if (!any) {
			return null;
		}
		if (full) {
			return Shapes.block();
		}
		BitSetDiscreteVoxelShape discrete = new BitSetDiscreteVoxelShape(8, 8, 8);
		for (int y = 0; y < 8; y++) {
			long layer = layers[y];
			while (layer != 0) {
				int bit = Long.numberOfTrailingZeros(layer);
				layer &= layer - 1;
				discrete.fill(bit & 7, y, bit >>> 3);
			}
		}
		return new CubeVoxelShape(discrete);
	}
}
