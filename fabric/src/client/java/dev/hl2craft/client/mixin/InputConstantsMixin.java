package dev.hl2craft.client.mixin;

import com.mojang.blaze3d.platform.InputConstants;
import com.mojang.blaze3d.platform.Window;
import dev.hl2craft.client.HL2Client;
import dev.hl2craft.client.InputBridge;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Keyboard state and mouse capture come from HL2 while linked, not from SDL. */
@Mixin(InputConstants.class)
public abstract class InputConstantsMixin {
	@Inject(method = "isKeyDown", at = @At("HEAD"), cancellable = true)
	private static void hl2craft$isKeyDown(int key, CallbackInfoReturnable<Boolean> cir) {
		if (HL2Client.linked()) {
			cir.setReturnValue(InputBridge.isKeyDown(key));
		}
	}

	@Inject(method = "grabMouse", at = @At("HEAD"), cancellable = true)
	private static void hl2craft$grabMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (HL2Client.linked()) {
			ci.cancel();
		}
	}

	@Inject(method = "releaseMouse", at = @At("HEAD"), cancellable = true)
	private static void hl2craft$releaseMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (HL2Client.linked()) {
			ci.cancel();
		}
	}
}
