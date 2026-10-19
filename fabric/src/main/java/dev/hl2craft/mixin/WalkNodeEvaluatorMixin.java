package dev.hl2craft.mixin;

import dev.hl2craft.world.HL2Collision;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.pathfinder.PathType;
import net.minecraft.world.level.pathfinder.PathfindingContext;
import net.minecraft.world.level.pathfinder.WalkNodeEvaluator;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Teaches Minecraft mob AI pathfinding that Half-Life 2 collision geometry is walkable ground,
 * not empty air / cliffs. This enables Creepers, Zombies, Skeletons, and animals to navigate
 * and walk towards players and NPCs across HL2 levels.
 */
@Mixin(WalkNodeEvaluator.class)
public abstract class WalkNodeEvaluatorMixin {

	@Inject(
		method = "getPathTypeStatic(Lnet/minecraft/world/level/pathfinder/PathfindingContext;Lnet/minecraft/core/BlockPos$MutableBlockPos;)Lnet/minecraft/world/level/pathfinder/PathType;",
		at = @At("RETURN"),
		cancellable = true
	)
	private static void hl2craft$injectHL2PathType(
		PathfindingContext context, BlockPos.MutableBlockPos pos, CallbackInfoReturnable<PathType> cir
	) {
		PathType type = cir.getReturnValue();

		// Check if position itself contains an HL2 obstacle or partial floor
		VoxelShape currentShape = HL2Collision.shapeAt(pos);
		if (currentShape != null && !currentShape.isEmpty()) {
			double maxY = currentShape.max(Direction.Axis.Y);
			double minY = currentShape.min(Direction.Axis.Y);
			double h = maxY - minY;
			if (h > 0.6 || maxY > 0.6) {
				cir.setReturnValue(PathType.BLOCKED);
				return;
			}
			// Low shape inside pos (step, slab, brush top <= 0.6) is walkable floor
			cir.setReturnValue(PathType.WALKABLE);
			return;
		}

		// If vanilla considers the node OPEN (air), check if there is HL2 ground beneath
		if (type == PathType.OPEN) {
			BlockPos below = pos.below();
			VoxelShape groundShape = HL2Collision.shapeAt(below);
			if (groundShape != null && !groundShape.isEmpty()) {
				cir.setReturnValue(PathType.WALKABLE);
			}
		}
	}

	@Inject(
		method = "getFloorLevel(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)D",
		at = @At("RETURN"),
		cancellable = true
	)
	private static void hl2craft$injectHL2FloorLevel(
		BlockGetter level, BlockPos pos, CallbackInfoReturnable<Double> cir
	) {
		VoxelShape curShape = HL2Collision.shapeAt(pos);
		if (curShape != null && !curShape.isEmpty()) {
			double maxY = curShape.max(Direction.Axis.Y);
			if (maxY <= 0.6) {
				cir.setReturnValue(pos.getY() + maxY);
				return;
			}
		}

		BlockPos below = pos.below();
		if (level.getBlockState(below).isAir()) {
			VoxelShape groundShape = HL2Collision.shapeAt(below);
			if (groundShape != null && !groundShape.isEmpty()) {
				cir.setReturnValue(below.getY() + groundShape.max(Direction.Axis.Y));
			}
		}
	}
}
