package dev.hl2craft.client.mixin;

import com.mojang.blaze3d.platform.Window;
import dev.hl2craft.client.HL2Client;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** The MC window is in the background while linked; HL2 has the real focus, so report focused. */
@Mixin(Window.class)
public abstract class WindowMixin {
	@Inject(method = "isFocused", at = @At("HEAD"), cancellable = true)
	private void hl2craft$focused(CallbackInfoReturnable<Boolean> cir) {
		if (HL2Client.linked()) {
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "isIconified", at = @At("HEAD"), cancellable = true)
	private void hl2craft$notIconified(CallbackInfoReturnable<Boolean> cir) {
		if (HL2Client.linked()) {
			cir.setReturnValue(false);
		}
	}
}
