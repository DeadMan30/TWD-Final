// SPDX-License-Identifier: GPL-2.0-only
// Export all discovered functions (address,size) from Ghidra to symbols/ghidra_functions.csv.
// Run from Ghidra Script Manager or analyzeHeadless:
//   analyzeHeadless <project_dir> <project_name> -process WalkingDead4.exe -noanalysis -postScript ExportFunctions.java symbols/ghidra_functions.csv
//@category TWD-Final

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

public class ExportFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = getScriptArgs().length > 0
            ? getScriptArgs()[0]
            : askFile("Output CSV", "Save").getAbsolutePath();

        List<Function> list = new ArrayList<>();
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            Function fn = it.next();
            if (!fn.isExternal() && !fn.isThunk()) {
                list.add(fn);
            }
        }
        list.sort(Comparator.comparing(f -> f.getEntryPoint().getOffset()));

        try (PrintWriter out = new PrintWriter(new FileWriter(outPath))) {
            out.println("address,size");
            for (Function fn : list) {
                long addr = fn.getEntryPoint().getOffset();
                long size = fn.getBody().getNumAddresses();
                out.printf("0x%09x,%d%n", addr, size);
            }
        }
        println("Exported " + list.size() + " functions to " + outPath);
    }
}
