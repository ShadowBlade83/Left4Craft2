package dev.hl2craft.mixin;

import dev.hl2craft.link.HL2Link;
import net.minecraft.server.MinecraftServer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(MinecraftServer.class)
public abstract class MinecraftServerFlightMixin {
	@Inject(method = "allowFlight", at = @At("HEAD"), cancellable = true)
	private void hl2craft$allowFlight(CallbackInfoReturnable<Boolean> cir) {
		if (HL2Link.active()) {
			cir.setReturnValue(true);
		}
	}
}
