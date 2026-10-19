package dev.hl2craft.mixin;

import dev.hl2craft.link.HL2Link;
import dev.hl2craft.link.Proto;
import dev.hl2craft.world.HL2Collision;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(Level.class)
public abstract class LevelMixin {
	@Inject(method = "setBlock(Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/level/block/state/BlockState;II)Z", at = @At("RETURN"))
	private void hl2craft$onSetBlock(BlockPos pos, BlockState state, int flags, int maxUpdateDepth, CallbackInfoReturnable<Boolean> cir) {
		if (HL2Link.active() && Boolean.TRUE.equals(cir.getReturnValue()) && state.isAir()) {
			HL2Collision.untrackPlacedBlock(pos);
			HL2Link.pushEvent(Proto.EV_BLOCK_BROKEN, 0, pos.getX(), pos.getY(), pos.getZ(), 0.0f, 0, 0);
		}
	}
}
