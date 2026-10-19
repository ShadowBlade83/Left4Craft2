package dev.hl2craft.client;

import java.nio.file.Path;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.network.chat.Component;

public class HL2SetupScreen extends Screen {
	private final Minecraft minecraft;
	private final Runnable onProceed;

	public HL2SetupScreen(Minecraft minecraft, Runnable onProceed) {
		super(Component.literal("HL2Craft: Настройка"));
		this.minecraft = minecraft;
		this.onProceed = onProceed;
	}

	@Override
	protected void init() {
		int cx = this.width / 2;
		int startY = 115;

		this.addRenderableWidget(
			Button.builder(Component.literal("1. Скачать ReShade (с поддержкой Add-on)"), b -> {
				HL2SetupManager.openUrl("https://reshade.me/#download");
			}).bounds(cx - 160, startY, 320, 20).build()
		);

		this.addRenderableWidget(
			Button.builder(Component.literal("2. Инструкция по установке"), b -> {
				HL2SetupManager.openUrl("https://reshade.me/");
			}).bounds(cx - 160, startY + 25, 320, 20).build()
		);

		this.addRenderableWidget(
			Button.builder(Component.literal("Указать папку Left 4 Dead 2 вручную"), b -> {
				Path selected = HL2SetupManager.browseForHL2Folder();
				if (selected != null) {
					this.rebuildWidgets();
				}
			}).bounds(cx - 160, startY + 50, 320, 20).build()
		);

		this.addRenderableWidget(
			Button.builder(Component.literal("Повторить проверку и запустить"), b -> {
				Path hl2 = HL2SetupManager.findHL2Dir();
				if (hl2 != null && HL2SetupManager.isReShadeInstalled(hl2)) {
					HL2SetupManager.installOrUpdateAddon(hl2);
					if (!HL2SetupManager.isHL2Running()) {
						HL2SetupManager.launchHL2(hl2);
					}
					onProceed.run();
				} else {
					this.rebuildWidgets();
				}
			}).bounds(cx - 160, startY + 80, 320, 20).build()
		);

		this.addRenderableWidget(
			Button.builder(Component.literal("Пропустить (только Minecraft)"), b -> {
				onProceed.run();
			}).bounds(cx - 160, startY + 105, 320, 20).build()
		);
	}

	@Override
	public void extractBackground(GuiGraphicsExtractor extractor, int mouseX, int mouseY, float partialTick) {
		this.extractMenuBackground(extractor);
	}

	@Override
	public void extractRenderState(GuiGraphicsExtractor extractor, int mouseX, int mouseY, float partialTick) {
		super.extractRenderState(extractor, mouseX, mouseY, partialTick);

		int cx = this.width / 2;
		extractor.centeredText(this.font, "§6§lLeft4Craft: Настройка интеграции Left 4 Dead 2", cx, 20, 0xFFFFFF);
		extractor.centeredText(this.font, "§7Для игры Стивом в Left 4 Dead 2 требуется ReShade 6.x с поддержкой Add-on", cx, 36, 0xAAAAAA);

		Path hl2Dir = HL2SetupManager.findHL2Dir();
		boolean reshade = HL2SetupManager.isReShadeInstalled(hl2Dir);

		String hl2Status = (hl2Dir != null)
			? "§a[+] Left 4 Dead 2 найден: " + hl2Dir.toString()
			: "§c[-] Left 4 Dead 2 НЕ найден! Нажмите 'Указать папку'.";
		extractor.centeredText(this.font, hl2Status, cx, 58, 0xFFFFFF);

		String reshadeStatus = reshade
			? "§a[+] ReShade установлен (d3d9.dll обнаружен)"
			: "§e[-] ReShade НЕ найден! Скачайте ReShade с поддержкой Add-on.";
		extractor.centeredText(this.font, reshadeStatus, cx, 74, 0xFFFFFF);

		extractor.centeredText(this.font, "§8При установке ReShade выберите left4dead2.exe, DirectX 9 и 'with full add-on support'.", cx, 92, 0x888888);
	}

	@Override
	public boolean shouldCloseOnEsc() {
		return false;
	}
}
