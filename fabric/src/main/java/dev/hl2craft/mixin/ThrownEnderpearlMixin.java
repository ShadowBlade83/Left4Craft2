package dev.hl2craft.mixin;

import dev.hl2craft.link.HL2Link;
import dev.hl2craft.link.Proto;
import net.minecraft.world.entity.projectile.throwableitemprojectile.ThrownEnderpearl;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(ThrownEnderpearl.class)
public abstract class ThrownEnderpearlMixin {
	@Inject(method = "onHit", at = @At("HEAD"))
	private void hl2craft$teleportPlayer(HitResult hitResult, CallbackInfo ci) {
		if (!HL2Link.active()) {
			return;
		}
		Vec3 loc = hitResult.getLocation();
		HL2Link.pushEvent(Proto.EV_TELEPORT, 0, (float) loc.x, (float) loc.y, (float) loc.z, 0.0f, 0, 0);
	}
}
