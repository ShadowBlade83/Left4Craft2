package dev.hl2craft.mixin;

import dev.hl2craft.world.HL2Collision;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.block.FallingBlock;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(FallingBlock.class)
public abstract class FallingBlockMixin {
	@Inject(method = "tick", at = @At("HEAD"), cancellable = true)
	private void hl2craft$restOnHL2(BlockState state, ServerLevel level, BlockPos pos, RandomSource random, CallbackInfo ci) {
		if (HL2Collision.hasGeometry(pos.below())) {
			ci.cancel();
		}
	}
}
