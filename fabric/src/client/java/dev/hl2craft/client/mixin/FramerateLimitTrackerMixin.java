package dev.hl2craft.client.mixin;

import com.mojang.blaze3d.platform.FramerateLimitTracker;
import dev.hl2craft.client.HL2Client;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Prevent Minecraft from reducing framerate when unfocused or AFK while linked to Half-Life 2. */
@Mixin(FramerateLimitTracker.class)
public abstract class FramerateLimitTrackerMixin {
	@Inject(method = "getFramerateLimit", at = @At("HEAD"), cancellable = true)
	private void hl2craft$unthrottledFps(CallbackInfoReturnable<Integer> cir) {
		if (HL2Client.linked()) {
			cir.setReturnValue(260);
		}
	}
}
