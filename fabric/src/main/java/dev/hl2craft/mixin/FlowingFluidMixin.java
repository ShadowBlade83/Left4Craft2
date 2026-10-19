package dev.hl2craft.mixin;

import dev.hl2craft.world.HL2Collision;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FlowingFluid;
import net.minecraft.world.level.material.Fluid;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(FlowingFluid.class)
public abstract class FlowingFluidMixin {
	@Inject(
		method = "canPassThroughWall",
		at = @At("HEAD"),
		cancellable = true
	)
	private static void hl2craft$noPassThroughHL2Geometry(
		Direction direction, BlockGetter level,
		BlockPos fromPos, BlockState fromBlockState,
		BlockPos toPos, BlockState toBlockState,
		CallbackInfoReturnable<Boolean> cir
	) {
		if (HL2Collision.hasGeometry(toPos)) {
			cir.setReturnValue(false);
		}
	}

	@Inject(
		method = "canHoldFluid",
		at = @At("HEAD"),
		cancellable = true
	)
	private static void hl2craft$noHoldFluidInHL2Geometry(
		BlockGetter level, BlockPos pos, BlockState state, Fluid fluid,
		CallbackInfoReturnable<Boolean> cir
	) {
		if (HL2Collision.hasGeometry(pos)) {
			cir.setReturnValue(false);
		}
	}

	@Inject(
		method = "isSolidFace",
		at = @At("HEAD"),
		cancellable = true
	)
	private void hl2craft$solidFaceOnHL2Geometry(
		BlockGetter level, BlockPos pos, Direction direction,
		CallbackInfoReturnable<Boolean> cir
	) {
		if (HL2Collision.hasGeometry(pos)) {
			cir.setReturnValue(true);
		}
	}
}
