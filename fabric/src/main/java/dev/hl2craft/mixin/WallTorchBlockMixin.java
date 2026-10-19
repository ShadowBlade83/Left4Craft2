package dev.hl2craft.mixin;

import dev.hl2craft.world.HL2Collision;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.level.LevelReader;
import net.minecraft.world.level.block.WallTorchBlock;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(WallTorchBlock.class)
public abstract class WallTorchBlockMixin {
	@Inject(method = "canSurvive", at = @At("HEAD"), cancellable = true)
	private void hl2craft$surviveOnHL2Wall(BlockState state, LevelReader level, BlockPos pos, CallbackInfoReturnable<Boolean> cir) {
		Direction facing = state.getValue(WallTorchBlock.FACING);
		BlockPos wallPos = pos.relative(facing.getOpposite());
		if (HL2Collision.hasGeometry(wallPos)) {
			cir.setReturnValue(true);
		}
	}
}
