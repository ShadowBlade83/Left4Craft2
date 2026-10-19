package dev.hl2craft.mixin;

import dev.hl2craft.HL2Craft;
import dev.hl2craft.link.HL2Link;
import dev.hl2craft.link.Proto;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageSource;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(ServerPlayer.class)
public abstract class ServerPlayerMixin {
	@Inject(method = "die", at = @At("HEAD"))
	private void hl2craft$diesInHL2(DamageSource source, CallbackInfo ci) {
		if (HL2Link.active()) {
			ServerPlayer self = (ServerPlayer) (Object) this;
			HL2Link.HostSnapshot snap = HL2Link.readHost();
			if (snap != null && snap.inPlayable()) {
				dev.hl2craft.world.HL2Collision.clearAllPlacedBlocks((net.minecraft.server.level.ServerLevel) self.level());
				HL2Link.pushEvent(Proto.EV_PLAYER_DIED, 0, 0, 0, 0, 0, 0, 0);
				HL2Craft.LOG.info("HL2Craft: player died in Minecraft; cleared placed blocks and killing in HL2");
			}
		}
	}
}
