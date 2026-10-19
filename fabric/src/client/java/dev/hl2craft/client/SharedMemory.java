package dev.hl2craft.client;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.VarHandle;
import java.nio.charset.StandardCharsets;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

public final class SharedMemory implements AutoCloseable {
	private static final int FILE_MAP_ALL_ACCESS = 0xF001F;
	private static final int PAGE_READWRITE = 4;

	private static final MethodHandle CREATE_FILE_MAPPING;
	private static final MethodHandle OPEN_FILE_MAPPING;
	private static final MethodHandle MAP_VIEW_OF_FILE;
	private static final MethodHandle UNMAP_VIEW_OF_FILE;
	private static final MethodHandle CLOSE_HANDLE;

	static {
		Linker linker = Linker.nativeLinker();
		SymbolLookup k32 = SymbolLookup.libraryLookup("kernel32", Arena.global());
		CREATE_FILE_MAPPING = linker.downcallHandle(
			k32.find("CreateFileMappingW").orElseThrow(),
			FunctionDescriptor.of(ADDRESS, ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS)
		);
		OPEN_FILE_MAPPING = linker.downcallHandle(
			k32.find("OpenFileMappingW").orElseThrow(),
			FunctionDescriptor.of(ADDRESS, JAVA_INT, JAVA_INT, ADDRESS)
		);
		MAP_VIEW_OF_FILE = linker.downcallHandle(
			k32.find("MapViewOfFile").orElseThrow(),
			FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG)
		);
		UNMAP_VIEW_OF_FILE = linker.downcallHandle(
			k32.find("UnmapViewOfFile").orElseThrow(),
			FunctionDescriptor.of(JAVA_INT, ADDRESS)
		);
		CLOSE_HANDLE = linker.downcallHandle(
			k32.find("CloseHandle").orElseThrow(),
			FunctionDescriptor.of(JAVA_INT, ADDRESS)
		);
	}

	public final MemorySegment handle;
	public final MemorySegment segment;
	public final long size;

	private SharedMemory(MemorySegment handle, MemorySegment segment, long size) {
		this.handle = handle;
		this.segment = segment;
		this.size = size;
	}

	public static SharedMemory createOrOpen(String name, long size) throws Throwable {
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment nameStr = arena.allocateFrom(name, StandardCharsets.UTF_16LE);
			int high = (int)(size >>> 32);
			int low = (int)size;
			MemorySegment h = (MemorySegment) CREATE_FILE_MAPPING.invokeExact(
				MemorySegment.ofAddress(-1L),
				MemorySegment.NULL,
				PAGE_READWRITE,
				high,
				low,
				nameStr
			);
			if (h.address() == 0) {
				h = (MemorySegment) OPEN_FILE_MAPPING.invokeExact(FILE_MAP_ALL_ACCESS, 0, nameStr);
			}
			if (h.address() == 0) {
				throw new RuntimeException("Failed to create or open shared memory " + name);
			}
			MemorySegment view = (MemorySegment) MAP_VIEW_OF_FILE.invokeExact(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
			if (view.address() == 0) {
				CLOSE_HANDLE.invokeExact(h);
				throw new RuntimeException("Failed to map view of file " + name);
			}
			return new SharedMemory(h, view.reinterpret(size), size);
		}
	}

	@Override
	public void close() {
		try {
			if (segment != null && segment.address() != 0) {
				UNMAP_VIEW_OF_FILE.invokeExact(segment);
			}
			if (handle != null && handle.address() != 0) {
				CLOSE_HANDLE.invokeExact(handle);
			}
		} catch (Throwable ignored) {
		}
	}
}
