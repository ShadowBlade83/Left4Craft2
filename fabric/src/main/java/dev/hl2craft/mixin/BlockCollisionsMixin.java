package dev.hl2craft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.hl2craft.world.HL2Collision;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockCollisions;
import net.minecraft.world.level.CollisionGetter;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.shapes.CollisionContext;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Adds HL2's geometry to every block-collision query dynamically.
 * Vanilla movement, step-up, onGround, fall damage, arrows, and TNT run unchanged against it.
 * The world itself remains pure AIR — zero barrier blocks!
 */
@Mixin(BlockCollisions.class)
public abstract class BlockCollisionsMixin {
	@WrapOperation(
		method = "computeNext",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/phys/shapes/CollisionContext;getCollisionShape(Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/CollisionGetter;Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/phys/shapes/VoxelShape;"
		)
	)
	private VoxelShape hl2craft$addHL2Shape(
		CollisionContext context, BlockState state, CollisionGetter level, BlockPos pos, Operation<VoxelShape> original
	) {
		VoxelShape blockShape = original.call(context, state, level, pos);
		if (state.is(net.minecraft.world.level.block.Blocks.BARRIER)) {
			blockShape = Shapes.empty();
		}
		VoxelShape hl2 = HL2Collision.shapeAt(pos);
		if (hl2 == null) {
			return blockShape;
		}
		if (context instanceof net.minecraft.world.phys.shapes.EntityCollisionContext entityContext
			&& entityContext.getEntity() instanceof net.minecraft.world.entity.player.Player) {
			dev.hl2craft.link.HL2Link.HostSnapshot snap = dev.hl2craft.link.HL2Link.readHost();
			if (snap != null && snap.hl2PhysicsOverride()) {
				return blockShape;
			}
		}
		return blockShape.isEmpty() ? hl2 : Shapes.or(blockShape, hl2);
	}
}
