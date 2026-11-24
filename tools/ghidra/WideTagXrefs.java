// Args: <outDir> <TAG>...  Finds NUL-terminated UTF-16 tags (e.g. NET) in memory, prints xrefs,
// and decompiles each referencing function into outDir/<program>/tag_<TAG>/
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.*;
import java.util.*;
public class WideTagXrefs extends GhidraScript {
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram);
        long base = currentProgram.getImageBase().getOffset();
        for (int i = 1; i < a.length; i++) {
            String t = a[i];
            byte[] pat = new byte[(t.length() + 2) * 2];           // \0\0 + tag + \0\0 (preceding NUL)
            for (int k = 0; k < t.length(); k++) pat[2 + k * 2] = (byte) t.charAt(k);
            File out = new File(a[0], currentProgram.getName() + "/tag_" + t); out.mkdirs();
            Set<Function> done = new HashSet<>();
            Address s = currentProgram.getMinAddress();
            while (true) {
                Address h = currentProgram.getMemory().findBytes(s, pat, null, true, monitor);
                if (h == null) break;
                Address str = h.add(2);
                for (Reference r : getReferencesTo(str)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    println("XREF " + currentProgram.getName() + " L\"" + t + "\" from +" + Long.toHexString(r.getFromAddress().getOffset() - base) + " in " + (f == null ? "<none>" : f.getName() + "@+" + Long.toHexString(f.getEntryPoint().getOffset() - base)));
                    if (f != null && done.add(f)) {
                        DecompileResults res = dec.decompileFunction(f, 60, monitor);
                        if (res.decompileCompleted()) try (PrintWriter w = new PrintWriter(new File(out, f.getName() + ".c"))) { w.print(res.getDecompiledFunction().getC()); }
                    }
                }
                s = h.add(1);
            }
        }
        dec.dispose();
    }
}
