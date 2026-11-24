// Args: <outDir>. Decompiles every function of the current program into
// outDir/<program>/all/<function>.c, for grepping. Keep the output outside the repository.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.Function;
import java.io.*;

public class DecompileAll extends GhidraScript {
    @Override public void run() throws Exception {
        File out = new File(getScriptArgs()[0], currentProgram.getName() + "/all"); out.mkdirs();
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram);
        int n = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.isThunk() || f.isExternal()) continue;
            DecompileResults res = dec.decompileFunction(f, 60, monitor);
            if (!res.decompileCompleted()) continue;
            try (PrintWriter w = new PrintWriter(new File(out, f.getName() + ".c"))) { w.print(res.getDecompiledFunction().getC()); }
            n++;
        }
        dec.dispose();
        println("OUT decompiled " + n + " functions");
    }
}
