package dev.hl2craft.mixin;

import dev.hl2craft.world.HL2Collision;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.LevelReader;
import net.minecraft.world.level.block.BaseTorchBlock;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(BaseTorchBlock.class)
public abstract class TorchBlockMixin {
	@Inject(method = "canSurvive", at = @At("HEAD"), cancellable = true)
	private void hl2craft$surviveOnHL2Floor(BlockState state, LevelReader level, BlockPos pos, CallbackInfoReturnable<Boolean> cir) {
		if (HL2Collision.hasGeometry(pos.below())) {
			cir.setReturnValue(true);
		}
	}
}
