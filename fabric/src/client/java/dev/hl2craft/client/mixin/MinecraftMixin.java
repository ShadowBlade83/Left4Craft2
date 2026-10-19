package dev.hl2craft.client.mixin;

import dev.hl2craft.client.HL2Client;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Minecraft.class)
public abstract class MinecraftMixin {
	@Shadow public int missTime;

	@Inject(method = "runTick", at = @At("HEAD"))
	private void hl2craft$beginFrame(boolean advanceGameTime, CallbackInfo ci) {
		HL2Client.beginFrame();
	}

	@Inject(method = "renderFrame", at = @At("HEAD"))
	private void hl2craft$beginRenderFrame(boolean advanceGameTime, CallbackInfo ci) {
		HL2Client.beginFrame();
	}

	@Inject(
		method = "renderFrame",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/GameRenderer;render()V", shift = At.Shift.AFTER)
	)
	private void hl2craft$afterRender(boolean advanceGameTime, CallbackInfo ci) {
		HL2Client.afterRender();
	}

	@Inject(method = "continueAttack", at = @At("HEAD"))
	private void hl2craft$clearMissTime(boolean down, CallbackInfo ci) {
		if (HL2Client.linked() && down) {
			this.missTime = 0;
		}
	}
}
