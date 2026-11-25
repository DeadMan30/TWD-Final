// SPDX-License-Identifier: GPL-2.0-only
// Export address,size of every real function in the .text block of WalkingDead4.exe.
//
//   analyzeHeadless ghidra_projects TWD-Final -process WalkingDead4.exe -noanalysis ^
//       -scriptPath tools/ghidra -postScript ExportFunctions.java symbols/ghidra_functions.csv
//@category TWD-Final

import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class ExportFunctions extends GhidraScript {
    private static final long IMAGE_BASE = 0x140000000L;

    @Override
    public void run() throws Exception {
        String outPath = getScriptArgs().length > 0
            ? getScriptArgs()[0]
            : askFile("Output CSV", "Save").getAbsolutePath();

        long base = currentProgram.getImageBase().getOffset();
        if (base != IMAGE_BASE) {
            throw new IllegalStateException(String.format(
                "image base is 0x%x, expected 0x%x", base, IMAGE_BASE));
        }
        Memory mem = currentProgram.getMemory();
        MemoryBlock text = mem.getBlock(".text");
        if (text == null) {
            throw new IllegalStateException("no .text block found");
        }

        List<long[]> rows = new ArrayList<>();
        int outside = 0, thunks = 0, external = 0, empty = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.isExternal()) { external++; continue; }
            if (f.isThunk()) { thunks++; continue; }
            Address entry = f.getEntryPoint();
            if (!text.contains(entry)) { outside++; continue; }
            long size = f.getBody().getNumAddresses();
            if (size < 1) { empty++; continue; }
            rows.add(new long[] { entry.getOffset(), size });
        }
        rows.sort((a, b) -> Long.compareUnsigned(a[0], b[0]));

        try (PrintWriter out = new PrintWriter(outPath, "UTF-8")) {
            out.print("address,size\n");
            for (long[] r : rows) {
                out.print(String.format("0x%09x,%d\n", r[0], r[1]));
            }
        }
        println(String.format(".text %s-%s", text.getStart(), text.getEnd()));
        println("EXPORT functions=" + rows.size() + " thunks_skipped=" + thunks
            + " external_skipped=" + external + " outside_text_skipped=" + outside
            + " empty_skipped=" + empty);
    }
}
