package dev.hl2craft.mixin;

import dev.hl2craft.link.HL2Link;
import dev.hl2craft.link.Proto;
import net.minecraft.world.level.ServerExplosion;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(ServerExplosion.class)
public abstract class ServerExplosionMixin {
	@Inject(method = "explode", at = @At("RETURN"))
	private void hl2craft$tellHL2(CallbackInfoReturnable<Integer> cir) {
		if (!HL2Link.active()) {
			return;
		}
		ServerExplosion self = (ServerExplosion) (Object) this;
		var center = self.center();
		HL2Link.pushEvent(Proto.EV_EXPLOSION, 0, (float) center.x, (float) center.y, (float) center.z, self.radius(), 0, 0);
	}
}
