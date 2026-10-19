package dev.hl2craft.mixin;

import dev.hl2craft.world.HL2Clip;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.entity.projectile.ProjectileDeflection;
import net.minecraft.world.phys.HitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(Projectile.class)
public abstract class ProjectileMixin {
	@Shadow
	protected abstract void onHit(HitResult hitResult);

	@Inject(method = "hitTargetOrDeflectSelf", at = @At("HEAD"), cancellable = true)
	private void hl2craft$hitHL2(HitResult hitResult, CallbackInfoReturnable<ProjectileDeflection> cir) {
		if (hitResult instanceof HL2Clip.HL2HitResult) {
			this.onHit(hitResult);
			cir.setReturnValue(ProjectileDeflection.NONE);
		}
	}
}
