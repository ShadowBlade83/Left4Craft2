package dev.hl2craft.client;

import dev.hl2craft.link.HL2Link;
import dev.hl2craft.link.Proto;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.screens.PauseScreen;
import net.minecraft.client.input.KeyEvent;
import net.minecraft.client.input.MouseButtonInfo;
import org.lwjgl.sdl.SDLKeyboard;

public final class InputBridge {
	private static final boolean[] KEYS = new boolean[512];
	private static final boolean[] BUTTONS = new boolean[8];
	private static double cursorX, cursorY;
	private static int modifiers;

	private InputBridge() {
	}

	public static boolean isKeyDown(int scancode) {
		return scancode >= 0 && scancode < KEYS.length && KEYS[scancode];
	}

	public static void drain(Minecraft minecraft) {
		HL2Link.drainInput((type, code, a, b, c) -> dispatch(minecraft, type, code, a, b, c));
	}

	private static void dispatch(Minecraft minecraft, int type, int code, int a, int b, int c) {
		long handle = minecraft.getWindow().handle();
		switch (type) {
			case Proto.IN_KEY -> key(minecraft, handle, code, a != 0);
			case Proto.IN_MOUSE_BUTTON -> {
				int button = code;
				if (button >= 0 && button < BUTTONS.length) {
					BUTTONS[button] = a != 0;
				}
				minecraft.mouseHandler.onButton(handle, new MouseButtonInfo(button, modifiers), a != 0 ? 1 : 0);
			}
			case Proto.IN_SCROLL -> minecraft.mouseHandler.onScroll(handle, 0.0, a / 120.0);
			case Proto.IN_CURSOR -> {
				int fbW = minecraft.getWindow().getScreenWidth();
				int fbH = minecraft.getWindow().getScreenHeight();
				int viewW = HL2Client.appliedViewportW();
				int viewH = HL2Client.appliedViewportH();
				double scaledA = (viewW > 0 && fbW > 0) ? (a * (double) fbW / viewW) : a;
				double scaledB = (viewH > 0 && fbH > 0) ? (b * (double) fbH / viewH) : b;
				double dx = scaledA - cursorX;
				double dy = scaledB - cursorY;
				cursorX = scaledA;
				cursorY = scaledB;
				minecraft.mouseHandler.onMove(handle, scaledA, scaledB, dx, dy);
			}
			case Proto.IN_TEXT -> {
				if (minecraft.gui.screen() != null) {
					minecraft.keyboardHandler.textInput(handle, new String(Character.toChars(a)));
				}
			}
			case Proto.IN_RELEASE_ALL -> releaseAll();
			case Proto.IN_HURT -> hurt(minecraft, a / 20.0F);
			case Proto.IN_OPEN_MENU -> {
				if (minecraft.gui.screen() == null && minecraft.player != null) {
					releaseAll();
					minecraft.gui.setScreen(new PauseScreen(true));
				}
			}
			case Proto.IN_BREAK_BLOCK -> breakBlock(minecraft, a, b, c);
			case 99 -> {
				if (minecraft.player != null && minecraft.level != null) {
					if (code == 1) {
						minecraft.level.playLocalSound(
							minecraft.player.getX(), minecraft.player.getY(), minecraft.player.getZ(),
							net.minecraft.sounds.SoundEvents.PLAYER_ATTACK_CRIT,
							net.minecraft.sounds.SoundSource.PLAYERS, 1.0F, 1.0F, false
						);
						double px = a / 100.0;
						double py = b / 100.0;
						double pz = c / 100.0;
						for (int i = 0; i < 16; i++) {
							double vx = (Math.random() - 0.5) * 0.6;
							double vy = (Math.random() - 0.2) * 0.5;
							double vz = (Math.random() - 0.5) * 0.6;
							minecraft.level.addParticle(net.minecraft.core.particles.ParticleTypes.CRIT, px, py, pz, vx, vy, vz);
						}
					} else {
						minecraft.level.playLocalSound(
							minecraft.player.getX(), minecraft.player.getY(), minecraft.player.getZ(),
							net.minecraft.sounds.SoundEvents.PLAYER_ATTACK_KNOCKBACK,
							net.minecraft.sounds.SoundSource.PLAYERS, 0.85F, 1.0F, false
						);
					}
				}
			}
			default -> {}
		}
	}

	/** The infected wore a barricade block down in L4D2: it breaks in Minecraft too, with its drop. */
	private static void breakBlock(Minecraft minecraft, int x, int y, int z) {
		var server = minecraft.getSingleplayerServer();
		if (server == null) {
			return;
		}
		server.execute(() -> {
			var pos = new net.minecraft.core.BlockPos(x, y, z);
			var level = server.overworld();
			if (!level.getBlockState(pos).isAir()) {
				level.destroyBlock(pos, true);
			}
		});
	}

		private static void hurt(Minecraft minecraft, float damage) {
		var server = minecraft.getSingleplayerServer();
		if (minecraft.player == null || server == null) {
			return;
		}
		var uuid = minecraft.player.getUUID();
		server.execute(() -> {
			net.minecraft.server.level.ServerPlayer player = server.getPlayerList().getPlayer(uuid);
			if (player != null && !player.isCreative() && !player.isInvulnerable()) {
				player.hurtServer(player.level(), player.damageSources().generic(), Math.max(0.5F, damage));
			}
		});
	}

	private static void key(Minecraft minecraft, long handle, int scancode, boolean down) {
		if (scancode <= 0 || scancode >= KEYS.length) {
			return;
		}
		boolean wasDown = KEYS[scancode];
		KEYS[scancode] = down;
		updateModifiers();

		if (down && !wasDown && scancode == 65 && minecraft.player != null) { // F8: collision dump (Left4Craft)
			dev.hl2craft.world.HL2Collision.dumpAround(minecraft.player.blockPosition(), minecraft.player.position());
		}
				if (down && !wasDown) {
			if (minecraft.gui.screen() instanceof net.minecraft.client.gui.screens.inventory.AbstractContainerScreen<?>) {
				if (scancode == 8) { // 'E' closes container screen
					releaseAll();
					minecraft.gui.setScreen(null);
					return;
				}
			} else if (minecraft.gui.screen() == null && minecraft.player != null) {
				if (scancode == 8) { // 'E' - Inventory
					releaseAll();
					minecraft.gui.setScreen(new net.minecraft.client.gui.screens.inventory.InventoryScreen(minecraft.player));
					return;
				}
				if (scancode == 23) { // 'T'
					releaseAll();
					minecraft.gui.setScreen(new net.minecraft.client.gui.screens.ChatScreen("", false));
					return;
				}
				if (scancode == 56) { // '/'
					releaseAll();
					minecraft.gui.setScreen(new net.minecraft.client.gui.screens.ChatScreen("/", false));
					return;
				}
			}
		}

		int action = down ? (wasDown ? -1 : 1) : 0;
		int keycode = SDLKeyboard.SDL_GetKeyFromScancode(scancode, (short) modifiers, true);
		minecraft.keyboardHandler.keyPress(handle, action, new KeyEvent(scancode, keycode, modifiers));
	}

	private static void updateModifiers() {
		int m = 0;
		if (KEYS[225]) m |= 0x0001; // SDL_KMOD_LSHIFT
		if (KEYS[229]) m |= 0x0002; // SDL_KMOD_RSHIFT
		if (KEYS[224]) m |= 0x0040; // SDL_KMOD_LCTRL
		if (KEYS[228]) m |= 0x0080; // SDL_KMOD_RCTRL
		if (KEYS[226]) m |= 0x0100; // SDL_KMOD_LALT
		if (KEYS[230]) m |= 0x0200; // SDL_KMOD_RALT
		modifiers = m;
	}

	public static void releaseAll() {
		Minecraft minecraft = Minecraft.getInstance();
		long handle = minecraft.getWindow().handle();
		for (int sc = 0; sc < KEYS.length; sc++) {
			if (KEYS[sc]) {
				KEYS[sc] = false;
				updateModifiers();
				minecraft.keyboardHandler.keyPress(handle, 0, new KeyEvent(sc, SDLKeyboard.SDL_GetKeyFromScancode(sc, (short) 0, true), modifiers));
			}
		}
		for (int button = 0; button < BUTTONS.length; button++) {
			if (BUTTONS[button]) {
				BUTTONS[button] = false;
				minecraft.mouseHandler.onButton(handle, new MouseButtonInfo(button, 0), 0);
			}
		}
	}
}
