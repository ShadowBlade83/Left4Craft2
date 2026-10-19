package dev.hl2craft.mixin;

import dev.hl2craft.link.HL2Link;
import dev.hl2craft.link.Proto;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerPlayerGameMode;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(ServerPlayerGameMode.class)
public abstract class ServerPlayerGameModeMixin {
	@Inject(method = "destroyBlock", at = @At("RETURN"))
	private void hl2craft$onBlockDestroyed(BlockPos pos, CallbackInfoReturnable<Boolean> cir) {
		if (HL2Link.active() && Boolean.TRUE.equals(cir.getReturnValue())) {
			dev.hl2craft.world.HL2Collision.untrackPlacedBlock(pos);
			HL2Link.pushEvent(Proto.EV_BLOCK_BROKEN, 0, pos.getX(), pos.getY(), pos.getZ(), 0.0f, 0, 0);
		}
	}
}
