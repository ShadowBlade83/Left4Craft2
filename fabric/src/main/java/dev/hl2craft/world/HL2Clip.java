package dev.hl2craft.world;

import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;

public final class HL2Clip {
	private HL2Clip() {
	}

	public enum Use {
		PROJECTILE,
		PICK
	}

	public static class HL2HitResult extends BlockHitResult {
		public HL2HitResult(Vec3 location, Direction direction, BlockPos pos) {
			super(location, direction, pos, false);
		}
	}

	public static BlockHitResult refine(Vec3 from, Vec3 to, BlockHitResult vanilla) {
		return refine(from, to, vanilla, Use.PICK);
	}

	public static BlockHitResult refine(Vec3 from, Vec3 to, BlockHitResult vanilla, Use use) {
		double dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
		double len = Math.sqrt(dx * dx + dy * dy + dz * dz);
		if (len < 1e-4) {
			return vanilla;
		}

		int steps = Math.min((int) (len * 20), 200);
		BlockPos.MutableBlockPos cur = new BlockPos.MutableBlockPos();
		BlockPos.MutableBlockPos prev = new BlockPos.MutableBlockPos();

		double stepX = dx / steps;
		double stepY = dy / steps;
		double stepZ = dz / steps;

		double cx = from.x;
		double cy = from.y;
		double cz = from.z;

		long lastPosLong = Long.MIN_VALUE;

		for (int i = 0; i < steps; i++) {
			cx += stepX;
			cy += stepY;
			cz += stepZ;
			cur.set((int) Math.floor(cx), (int) Math.floor(cy), (int) Math.floor(cz));
			long posLong = cur.asLong();
			if (posLong == lastPosLong) {
				continue;
			}
			lastPosLong = posLong;

			net.minecraft.world.phys.shapes.VoxelShape shape = HL2Collision.shapeAt(cur);
			if (shape != null && !shape.isEmpty()) {
				BlockHitResult shapeHit = shape.clip(from, to, cur);
				if (shapeHit != null) {
					if (vanilla.getType() != HitResult.Type.MISS) {
						if (from.distanceToSqr(vanilla.getLocation()) <= from.distanceToSqr(shapeHit.getLocation()) + 0.05) {
							return vanilla;
						}
					}
					Vec3 loc = shapeHit.getLocation();
					Direction dir = shapeHit.getDirection();
					if (use == Use.PICK) {
						double out = 0.25;
						BlockPos placePos = BlockPos.containing(
							loc.x + dir.getStepX() * out,
							loc.y + dir.getStepY() * out,
							loc.z + dir.getStepZ() * out
						);
						return new HL2HitResult(loc, dir, placePos);
					}
					return new HL2HitResult(loc, dir, cur.immutable());
				}
			}
		}

		return vanilla;
	}

	public static boolean isOccluded(Vec3 from, Vec3 to) {
		double dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
		double len = Math.sqrt(dx * dx + dy * dy + dz * dz);
		if (len < 1.0) {
			return false;
		}

		int steps = Math.min((int) (len * 8), 120);
		BlockPos.MutableBlockPos cur = new BlockPos.MutableBlockPos();
		BlockPos.MutableBlockPos target = new BlockPos.MutableBlockPos((int) Math.floor(to.x), (int) Math.floor(to.y), (int) Math.floor(to.z));

		double stepX = dx / steps;
		double stepY = dy / steps;
		double stepZ = dz / steps;

		double cx = from.x;
		double cy = from.y;
		double cz = from.z;

		for (int i = 0; i < steps - 1; i++) {
			cx += stepX;
			cy += stepY;
			cz += stepZ;
			cur.set((int) Math.floor(cx), (int) Math.floor(cy), (int) Math.floor(cz));
			if (cur.equals(target)) {
				break;
			}
			net.minecraft.world.phys.shapes.VoxelShape shape = HL2Collision.shapeAt(cur);
			if (shape != null && !shape.isEmpty() && shape.clip(from, to, cur) != null) {
				return true;
			}
		}
		return false;
	}
}
