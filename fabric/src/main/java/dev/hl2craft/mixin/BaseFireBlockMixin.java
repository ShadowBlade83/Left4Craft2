package dev.hl2craft.mixin;

import dev.hl2craft.world.HL2Collision;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.BaseFireBlock;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(BaseFireBlock.class)
public abstract class BaseFireBlockMixin {
	@Inject(method = "canBePlacedAt", at = @At("HEAD"), cancellable = true)
	private static void hl2craft$placeOnHL2Floor(Level level, BlockPos pos, Direction direction, CallbackInfoReturnable<Boolean> cir) {
		if (level.getBlockState(pos).isAir() && HL2Collision.hasGeometry(pos.below())) {
			cir.setReturnValue(true);
		}
	}
}
