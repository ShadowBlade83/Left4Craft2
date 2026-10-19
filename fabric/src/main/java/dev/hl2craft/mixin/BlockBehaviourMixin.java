package dev.hl2craft.mixin;

import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockBehaviour;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.shapes.CollisionContext;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(BlockBehaviour.class)
public abstract class BlockBehaviourMixin {
	@Inject(method = "getShape", at = @At("HEAD"), cancellable = true)
	private void hl2craft$noBarrierOutline(BlockState state, BlockGetter level, BlockPos pos, CollisionContext context, CallbackInfoReturnable<VoxelShape> cir) {
		if (state.is(Blocks.BARRIER)) {
			cir.setReturnValue(Shapes.empty());
		}
	}

	@Inject(method = "getCollisionShape", at = @At("HEAD"), cancellable = true)
	private void hl2craft$noBarrierCollision(BlockState state, BlockGetter level, BlockPos pos, CollisionContext context, CallbackInfoReturnable<VoxelShape> cir) {
		if (state.is(Blocks.BARRIER)) {
			cir.setReturnValue(Shapes.empty());
		}
	}

	@Inject(method = "canBeReplaced", at = @At("HEAD"), cancellable = true)
	private void hl2craft$barrierReplaceable(BlockState state, net.minecraft.world.item.context.BlockPlaceContext context, CallbackInfoReturnable<Boolean> cir) {
		if (state.is(Blocks.BARRIER)) {
			cir.setReturnValue(true);
		}
	}
}
