// Args: <outDir> <needle1> [needle2 ...]. Each needle is a case-insensitive substring of a defined string.
// Prints string -> referencing function, and decompiles each referencing function to outDir/<program>/<func>.c
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.*;
import java.util.*;
public class StringXrefs extends GhidraScript {
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        File out = new File(a[0], currentProgram.getName()); out.mkdirs();
        List<String> needles = new ArrayList<>();
        for (int i = 1; i < a.length; i++) needles.add(a[i].toLowerCase());
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram);
        Set<Function> done = new HashSet<>();
        long base = currentProgram.getImageBase().getOffset();
        for (Data d : currentProgram.getListing().getDefinedData(true)) {
            if (!d.hasStringValue()) continue;
            String s = String.valueOf(d.getValue()); String l = s.toLowerCase();
            boolean hit = false; for (String n : needles) if (l.contains(n)) { hit = true; break; }
            if (!hit) continue;
            for (Reference r : getReferencesTo(d.getAddress())) {
                Function f = getFunctionContaining(r.getFromAddress());
                String fn = f == null ? "<none>" : f.getName() + "@+" + Long.toHexString(f.getEntryPoint().getOffset() - base);
                println("XREF \"" + s + "\" from +" + Long.toHexString(r.getFromAddress().getOffset() - base) + " in " + fn);
                if (f != null && done.add(f)) {
                    DecompileResults res = dec.decompileFunction(f, 60, monitor);
                    if (res.decompileCompleted()) try (PrintWriter w = new PrintWriter(new File(out, f.getName() + ".c"))) { w.print(res.getDecompiledFunction().getC()); }
                }
            }
        }
        dec.dispose();
    }
}
