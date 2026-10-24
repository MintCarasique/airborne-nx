#!/usr/bin/env python3
"""Second pass of the Spanish -> English rename map: English word order.

Reads rename_map.tsv (spanish column), words.tsv, overrides.tsv (hand-made
fixes: spanish<TAB>english) and writes rename_map_reviewed.tsv.

The first pass translated word by word and kept the Spanish order
("RegistroDibujo" -> "RegisterDraw"). This pass:
  * fixes word senses (WORD_FIX below),
  * reorders noun phrases to English order (head noun last): the tokens of a
    phrase are reversed, "X de Y" becomes "Y X", prepositions (with, without,
    per, in, to, ...) split phrases and stay in place, units (ns, ms, bytes)
    and qualifiers (total, max) go to the end, prefixes (g_, k, nvk_, ...) stay
    in front, a leading verb stays in front;
  * applies overrides.tsv last (hand-checked against the source).
Collisions are NOT auto-resolved; `translate_ids.py --map` reports them.

Run: python -I review_ids.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import translate_ids as T  # noqa: E402

MAP_IN = os.path.join(HERE, "rename_map.tsv")
MAP_OUT = os.path.join(HERE, "rename_map_reviewed.tsv")
OVERRIDES = os.path.join(HERE, "overrides.tsv")

# Word-sense fixes (lowercase Spanish -> English, "" drops the word).
WORD_FIX = {
    "informe": "report", "informes": "reports",
    "tiron": "hitch", "anotar": "track", "anotado": "tracked", "anotada": "tracked",
    "borrado": "clear", "borrados": "clears", "borrar": "clear",
    "borradas": "clears",
    "grabar": "record", "grabado": "recorded", "grabada": "recorded",
    "registro": "reg", "registros": "regs",
    "registrar": "register", "registrado": "registered",
    "huella": "fingerprint", "huellas": "fingerprints",
    "testigo": "probe", "relevo": "handoff", "preparador": "preparer",
    "ejecutor": "executor", "rechazar": "reject", "rechazados": "rejected",
    "pendiente": "pending", "pendientes": "pending",
    "ahora": "now", "frontal": "front", "frontales": "fronts",
    "fotograma": "frame", "fotogramas": "frames", "foto": "snapshot",
    "fotos": "snapshots", "ranura": "slot", "ranuras": "slots",
    "pedido": "request", "pedidos": "requests", "peticion": "request",
    "pedir": "request", "mosaico": "tile", "estencil": "stencil",
    "valla": "fence", "vallas": "fences", "hilo": "thread",
    "anillo": "ring", "cielo": "sky", "objeto": "object", "objetos": "objects",
    "oclusion": "occlusion", "resplandor": "glow", "desenfoque": "blur",
    "cerrojo": "mutex", "candado": "lock",
    "espera": "wait", "esperas": "waits", "esperar": "wait",
    "esperando": "waiting", "mirando": "peeking", "mira": "peek",
    "mirar": "peek", "mirados": "peeked", "mirada": "peeked",
    "juego": "game", "delante": "ahead", "mundo": "world",
    "subida": "upload", "subidas": "uploads", "subir": "upload",
    "subidos": "uploaded", "escalable": "scalable",
    "cabe": "fits", "util": "usable", "utiles": "usable",
    "nivel": "level", "niveles": "levels", "reloj": "clock",
    "y": "y", "x": "x", "z": "z",
    "diag": "diag", "sc": "sc", "d": "d", "g": "g", "s": "s",
    "i": "i", "n": "n", "t": "t", "l": "l", "b": "b", "c": "c", "p": "p",
    "it": "it", "dib": "draw", "inm": "imm", "im": "im", "sm": "sm",
    "en": "in", "a": "to", "al": "on", "con": "with", "sin": "without",
    "entre": "between", "hasta": "until", "desde": "from", "tras": "after",
    "para": "for", "por": "per", "antes": "before", "despues": "after",
    "como": "as", "que": "that", "si": "if", "no": "no", "es": "is",
    "hay": "has", "tiene": "has", "admite": "supports", "uno": "one",
    "dos": "two", "cada": "every", "solo": "only", "todo": "all",
    "todos": "all", "todas": "all", "de": "of", "del": "of",
    "la": "", "el": "", "las": "", "los": "", "un": "", "una": "",
    "datos": "data", "dato": "datum", "origen": "source",
    "vuelo": "flight", "fuente": "source", "comprobar": "check",
    "ahorrados": "saved", "ayudadas": "assisted", "ayudados": "assisted",
    "ayudando": "helping", "perezosa": "lazy", "perezoso": "lazy",
    "temprana": "early", "tempranas": "early", "temprano": "early",
    "vegetacion": "vegetation", "sombras": "shadows", "sombra": "shadow",
    "presentados": "presented", "presentado": "presented",
    "presente": "present", "presentar": "present",
    "copia": "copy", "copias": "copies", "creados": "created",
    "creadas": "created", "creado": "created", "creada": "created",
    "soltar": "release", "soltadas": "released", "sueltas": "released",
    "soltando": "releasing", "liberar": "free", "tomar": "take",
    "visibilidad": "visibility", "aviso": "warning", "avisos": "warnings",
    "avisar": "warn", "avisado": "warned",
    "recogidos": "collected", "recogidas": "collected", "recoger": "collect",
    "recogiendo": "collecting", "tirones": "hitches", "tiradas": "dropped",
    "vacio": "empty", "vacia": "empty", "vacias": "empty", "vacios": "empty",
    "diferido": "deferred", "diferidos": "deferred", "diferir": "defer",
    "invalido": "invalid", "valido": "valid", "valida": "valid",
    "validas": "valid", "entera": "int", "enteras": "ints", "entero": "int",
    "iguales": "equal", "igual": "equal", "identica": "identical",
    "fingida": "faked", "reservas": "reserves",
    "nucleo": "core", "ventana": "window", "ventanas": "windows",
    "carpeta": "folder", "fichero": "file", "ficheros": "files",
    "viejo": "old", "vieja": "old", "viejos": "old",
    "ruta": "path", "pausa": "pause", "kill": "kill", "kills": "kills",
    "baldosas": "tiles", "cubos": "cubes", "cubeta": "bucket",
    "cubetas": "buckets", "almacen": "store", "domo": "dome",
    "goteo": "trickle", "agua": "water", "bien": "ok",
    "marca": "mark", "marcas": "marks", "marcador": "marker",
    "marcadores": "markers", "cifras": "figures", "minucias": "details",
    "suave": "smooth", "natural": "natural", "barato": "cheap",
    "rapido": "fast", "rapida": "fast", "rapidas": "fast",
    "fija": "fixed", "fijo": "fixed", "llena": "full", "llenos": "full",
    "lleno": "full", "limpio": "clean", "limpia": "clean",
    "corte": "cut", "cortes": "cuts", "tijera": "scissor",
    "perdidas": "lost", "perdidos": "lost", "perdida": "lost",
    "muertas": "dead", "muerta": "dead", "vivas": "alive", "viva": "alive",
    "vivo": "live", "invisible": "invisible", "invisibles": "invisible",
    "aparte": "separately", "hechas": "done", "hechos": "done",
    "hecha": "done", "hecho": "done",
    "diagnostico": "diagnostic", "diagnosticos": "diagnostics",
    "habilitado": "enabled", "encendido": "enabled", "encendida": "enabled",
    "apagado": "off", "apagada": "off", "apagar": "disable",
    "activa": "active", "activo": "active", "activas": "active",
    "abierta": "open", "abierto": "open", "cerrada": "closed",
    "ultimo": "last", "ultima": "last", "primer": "first",
    "primero": "first", "primera": "first", "primeros": "first",
    "siguiente": "next", "anterior": "previous", "previo": "previous",
    "previa": "previous", "previos": "previous", "previas": "previous",
    "otro": "other", "otra": "other", "misma": "same", "mismo": "same",
    "nuevo": "new", "nueva": "new", "nuevos": "new", "nuevas": "new",
    "actual": "current", "maximo": "max", "maxima": "max", "minimo": "min",
    "minima": "min", "total": "total", "totales": "total",
    "cruda": "raw", "crudo": "raw", "canonica": "canonical",
    "resuelta": "resolved", "resueltas": "resolved", "resuelto": "resolved",
    "escrita": "written", "escritas": "written", "escrito": "written",
    "leido": "read", "leida": "read", "leidos": "read", "leidas": "read",
    "copiado": "copied", "copiadas": "copied", "copiada": "copied",
    "alternancia": "toggle", "alternar": "toggle",
    "escritura": "write", "escrituras": "writes",
    "lectura": "read", "lecturas": "reads", "lee": "reads",
    "bufer": "buffer", "bufers": "buffers",
    "sesgo": "bias", "peor": "worst", "tardias": "late",
    "limite": "limit", "entre": "between",
}

VERBS = set("""
leer escribir crear anotar copiar comprobar diferir borrar terminar rechazar
alternar esperar destruir contar grabar resolver guardar reutilizar presentar
soltar invalidar empezar pedir obtener enviar parar decidir sumar inicializar
medir aplicar apagar liberar olvidar comparar tomar desenmosaicar dibujar
restaurar preparar avisar cargar identificar aplazar encolar compensar subir
emitir cambiar recoger verificar trazar omitir rellenar planear publicar
vigilar escalar quitar reparar mirar registrar refrescar habilitar saltar
asegurar ejecutar elegir extender iniciar marcar pintar podar recalcular
recordar rehacer sacar devolver ocultar aprender cerrar componer descartar
despertar emparejar entregar expulsar fijar hacer procesar limpiar mover
serializar precalentar abrir activar calcular intercambiar rotar usar
es hay tiene admite
poner permitir exigir buscar anadir arrancar ayudar alinear avanzar cortar
cronometrar completar empaquetar encoger enlazar girar ignorar intentar
reiniciar reservar retirar revisar validar forzar canonizar precargar
recoger
""".split())

PREPS = {"con", "sin", "por", "en", "a", "entre", "hasta", "desde", "tras",
         "para", "antes", "despues", "al", "como", "que", "si"}
# stay at the front of their phrase (Spanish pre-nominal adjectives)
PRENOM = {"ultimo", "ultima", "ultimos", "ultimas", "primer", "primera",
          "primero", "primeros", "siguiente", "otro", "otra", "misma", "mismo",
          "nuevo", "nueva", "nuevos", "nuevas", "cada", "solo", "todo",
          "sub", "peor", "mejor", "segundo", "su"}
# prefixes that stay in front of a name
PREFIXES = {"cielo", "compuesta", "frontal", "perezosa", "precalentado",
            "encuadre", "vegetacion", "sombra", "sombras", "resplandor", "set",
            "spec", "spirv", "gpu", "z", "l", "it", "sombraminimo", "g", "s", "sm", "im", "inm", "nvk", "diag", "eds", "nfsmw", "rex",
            "d", "l2", "fb", "fs", "tiron", "vk", "sw", "t", "i", "reg",
            "num", "n", "max", "no", "es", "hay", "tiene"}
# units / qualifiers that go to the end
UNITS = {"ns", "ms", "us", "mb", "kb", "bytes", "megas", "ticks"}
TAIL = {"alto", "ancho", "apagada", "apagado", "apagados", "encendida",
        "encendido", "previo", "previa", "previos", "previas", "cada", "ok",
        "primero", "ultimo", "ultima", "primera", "total", "max", "maximo", "min", "minimo", "informe", "informes",
        "out", "alternar", "s", "ns", "ms", "us", "mb", "kb", "bytes"}
AXIS = {"x", "y", "z", "i"}
KEEP_ORDER = {"mip", "mips"}


class Eng:
    def __init__(self):
        table = T.load_words()
        self.table = {k: v[0] for k, v in table.items()}
        self.table.update(WORD_FIX)
        with open(os.path.join(HERE, 'words_extra.tsv'), encoding='utf-8') as fh:
            for ln in fh:
                a = ln.rstrip('\r\n').split('\t')
                if len(a) == 2:
                    self.table[a[0]] = a[1]
        self.missing = set()

    def tr(self, w):
        if w in self.table:
            return self.table[w]
        self.missing.add(w)
        return w

    def is_english(self, w):
        e = self.tr(w)
        return e == w and w not in PREPS and w not in VERBS


def tokenize(ident):
    """Returns (lead_underscores, tokens, trail_underscores, style).
    tokens are (lowercase word, original piece)."""
    lead = len(ident) - len(ident.lstrip("_"))
    trail = len(ident) - len(ident.rstrip("_"))
    core = ident[lead:len(ident) - trail] if trail else ident[lead:]
    parts = [p for p in core.split("_") if p != ""]
    toks = []
    for p in parts:
        for t in T.CAMEL.findall(p):
            toks.append(t)
    return lead, toks, trail


def style_of(ident, kind):
    core = ident.strip("_")
    if kind == "Macro" or (core.upper() == core and any(c.isalpha() for c in core)):
        return "macro"
    if "_" in core:
        if core[:1].isupper() and kind in ("Type",):
            return "mixed"
        return "snake"
    if len(core) > 1 and core[0] == "k" and core[1].isupper():
        return "k"
    if core[0].isupper():
        return "camel"
    return "snake"


def reorder(eng, lw):
    """lw: lowercase Spanish tokens. Returns the tokens in English order."""
    trailing_of = False
    if len(lw) > 1 and lw[-1] == "de":
        lw = lw[:-1]
        trailing_of = True
    merged = []
    for w in lw:
        if merged and merged[-1] == "sombra" and w == "minimo":
            merged[-1] = "sombraminimo"
        else:
            merged.append(w)
    lw = merged
    units = []
    for i, w in enumerate(lw):
        if units and (w in AXIS or w.isdigit()) and units[-1][0] not in PREPS                 and units[-1][0] not in VERBS and units[-1][0] != "de":
            if not (w == "y" and i + 1 < len(lw) and lw[i + 1] in VERBS):
                units[-1].append(w)
                continue
        units.append([w])
    ws = units
    front = []
    while len(ws) > 1 and ws[0][0] in PREFIXES:
        front.append(ws.pop(0))
    verb = None
    if len(ws) > 1 and ws[0][0] in VERBS:
        verb = ws.pop(0)
    lead_units = []
    while len(ws) > 1 and ws[0][0] in UNITS:
        lead_units.append(ws.pop(0))
    tail = []
    while len(ws) > 1 and ws[-1][0] in TAIL:
        tail.insert(0, ws.pop())
    res = []
    chain = [[]]
    for u in ws:
        w = u[0]
        if w in ("de", "del"):
            chain.append([])
        elif w in PREPS:
            res.append(chain)
            res.append(u)
            chain = [[]]
        else:
            chain[-1].append(u)
    res.append(chain)
    body = []
    for item in res:
        if item and isinstance(item[0], str):
            body.append(item)
            continue
        for seg in reversed(item):
            if not seg:
                continue
            seg = list(seg)
            pre = []
            while len(seg) > 1 and seg[0][0] in PRENOM:
                pre.append(seg.pop(0))
            grouped = []
            for u in seg:
                isen = eng.is_english(u[0])
                if grouped and isen and grouped[-1][1]:
                    grouped[-1][0].append(u)
                else:
                    grouped.append([[u], isen])
            seg_out = []
            for g, _ in reversed(grouped):
                seg_out.extend(g)
            body.extend(pre)
            body.extend(seg_out)
    final = front + ([verb] if verb else []) + body
    final += [u for u in tail if u[0] not in UNITS]
    final += [u for u in tail if u[0] in UNITS] + lead_units
    flat = []
    for u in final:
        flat.extend(u)
    if trailing_of:
        flat.append("de")
    return flat


def render(eng, ident, kind):
    lead, toks, trail = tokenize(ident)
    if not toks:
        return ident
    style = style_of(ident, kind)
    lw = [t.lower() for t in toks]
    # already English: every token maps to itself
    if all(eng.is_english(w) or w.isdigit() for w in lw):
        return ident
    kprefix = False
    if style == "k":
        # strip the k
        first = toks[0]
        if first == "k":
            kprefix = True
            lw = lw[1:]
        elif first[0] == "k":
            pass
    if style == "k" and not kprefix:
        # CAMEL regex splits "kRanuras" as 'k' + 'Ranuras'
        pass
    wordsES = reorder(eng, lw)
    en = []
    for w in wordsES:
        if w.isdigit():
            en.append(w)
            continue
        e = eng.tr(w)
        if e == "":
            continue
        en.append(e)
    # merge words that translate to several (e.g. "a la")
    parts = []
    for e in en:
        parts.extend(e.split("_") if "_" in e else [e])
    if not parts:
        return ident
    # glue digits/axis directly (scale_x stays scale_x; set4 stays set4)
    glued = []
    for p in parts:
        if p.isdigit() and glued:
            glued[-1] += p
        else:
            glued.append(p)
    parts = glued
    if style == "macro":
        s = "_".join(p.upper() for p in parts)
    elif style in ("camel", "k"):
        s = "".join(p[:1].upper() + p[1:] for p in parts)
        if kprefix:
            s = "k" + s
    else:
        s = "_".join(parts)
        # digits glued to the previous word in snake case (set4 -> set4)
        s = re.sub(r"_(\d+)", r"\1", s) if False else s
    return "_" * lead + s + "_" * trail


def main():
    eng = Eng()
    rows = []
    with open(MAP_IN, encoding="utf-8") as fh:
        next(fh)
        for line in fh:
            c = line.rstrip("\r\n").split("\t")
            c += [""] * (5 - len(c))
            rows.append(c)
    overrides = {}
    if os.path.exists(OVERRIDES):
        with open(OVERRIDES, encoding="utf-8") as fh:
            for line in fh:
                if not line.strip() or line.startswith("#"):
                    continue
                a, b = line.rstrip("\r\n").split("\t")[:2]
                overrides[a] = b
    out = []
    for sp, old, kind, occ, note in rows:
        if sp in overrides:
            en = overrides[sp]
        else:
            en = auto(eng, sp, kind)
        out.append((sp, en, kind, occ, note, old))
    with open(os.path.join(HERE, "cvars_es.tsv"), "w", encoding="utf-8",
              newline="\n") as fh:
        fh.write("spanish\tenglish_proposed\toccurrences\n")
        for sp, en, kind, occ, note, old in out:
            if kind == "Cvar":
                fh.write("%s\t%s\t%s\n" % (sp, en, occ))
    out = [r for r in out if r[2] != "Cvar"]
    with open(MAP_OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("spanish\tenglish\tkind\toccurrences\tcollision_note\tchanged\n")
        for sp, en, kind, occ, note, old in out:
            fh.write("%s\t%s\t%s\t%s\t%s\t%s\n" % (
                sp, en, kind, occ, "", "yes" if en != old else "no"))
    print("rows:", len(out), "overrides used:",
          sum(1 for r in rows if r[0] in overrides),
          "missing words:", sorted(eng.missing)[:20])


def auto(eng, sp, kind):
    # Cvars: keep the nfsmw_ prefix, translate the rest
    if kind == "Cvar":
        core = sp
        pre = ""
        m = re.match(r"(nfsmw_)(nativo_)?(.*)$", sp)
        if m:
            pre = "nfsmw_" + ("native_" if m.group(2) else "")
            core = m.group(3)
        return pre + render(eng, core, "Variable")
    return render(eng, sp, kind)


if __name__ == "__main__":
    main()
