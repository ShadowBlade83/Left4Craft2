package dev.hl2craft.client.mixin;

import dev.hl2craft.client.HL2Client;
import net.minecraft.client.MouseHandler;
import net.minecraft.client.input.MouseButtonInfo;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(MouseHandler.class)
public abstract class MouseHandlerMixin {
	@Shadow
	private boolean mouseGrabbed;

	@Inject(method = "isMouseGrabbed", at = @At("HEAD"), cancellable = true)
	private void hl2craft$isMouseGrabbed(CallbackInfoReturnable<Boolean> cir) {
		if (HL2Client.linked()) {
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "onButton", at = @At("HEAD"))
	private void hl2craft$forceGrabbedOnButton(long window, MouseButtonInfo buttonInfo, int action, CallbackInfo ci) {
		if (HL2Client.linked()) {
			this.mouseGrabbed = true;
		}
	}
}
