#!/usr/bin/env python3
"""Mechanical inventory of Spanish-language identifiers, strings and comments
in the C++/GLSL sources under rex/src/native (recursive).

Run:  python -I D:\\Games\\AirborneNX\\analysis\\native_es\\inventory.py [source_root]

Outputs (next to this script):
  identifiers.tsv      flagged (Spanish) identifiers, sorted by occurrences
  strings.tsv          string literals containing a Spanish word
  comments_count.tsv   per file: comment lines containing a Spanish word,
                       plus up to 5 sample lines

Kind priority for identifiers (first match wins):
  Cvar      first argument of REXCVAR_DEFINE_* (code, or string form)
  Type      CamelCase identifier following struct/class/enum/union/using/typedef
  Macro     defined via #define, or ALL_CAPS (also matches ALL_CAPS enum values)
  Function  followed by '(' somewhere
  Variable  otherwise
"""
import os
import re
import sys
import unicodedata
from collections import Counter, defaultdict

DEFAULT_ROOT = r"D:\Games\AirborneNX\rex\src\native"
OUT_DIR = os.path.dirname(os.path.abspath(__file__))

# Lowercase, accent-free Spanish words (roots, plurals and common forms used in
# graphics/engine/game code). Words that are identical in English and would
# produce heavy noise (color, video, error, normal, base, bit, red, motor, ...)
# are deliberately left out. "total", "version", "cache", "actual" and "ms" are
# included because they were requested, but they also occur in English code.
SPANISH_WORDS = """
dibujo dibujar dibujos dibujado dibujada dibujados dibujadas dibujando
destino destinos anillo anillos gancho ganchos nativo nativa nativos nativas
nativamente textura texturas fotograma fotogramas huella huellas
sombra sombras sombreador sombreadores sombreado sombreada sombreados sombreadora
cielo cielos resplandor resplandores reflejo reflejos reflejar vegetacion
obtener obtiene obtenido obtenida obtenidos obtenidas obtencion
crear creacion creado creada creados creadas creando crea
leer lectura lecturas lee leido escribir escritura escrituras escribe escrito escrita
rechazar rechazo rechazado rechazada rechazados rechazadas
emparejar emparejado emparejada emparejados emparejadas emparejamiento
presentar presentacion presentado presentada presentados presentadas presenta presente
paquete paquetes registro registros registrar registrado registrada
espera esperar esperando esperado esperada esperas tiron tirones
ajuste ajustes ajustar ajustado grafico graficos grafica graficas
sistema sistemas salida salidas salir rampa rampas entrada entradas entrar
pila pilas cola colas bucle bucles cuenta cuentas contar contador contadores contado
tamano tamanos tamanio tamanios ancho anchos alto altos altura alturas anchura
profundidad profundidades mezcla mezclas mezclar mezclado
muestra muestras muestrear muestreo muestreador muestreadores
cache caches vertice vertices indice indices
primitiva primitivas primitivo primitivos recorte recortes recortar
copia copias copiar copiado copiada limpiar limpio limpia limpieza limpiado
nuevo nueva nuevos nuevas viejo vieja viejos viejas
ultimo ultima ultimos ultimas primero primera primeros primeras
siguiente siguientes anterior anteriores previo previa actual actuales total totales
valor valores clave claves version versiones microcodigo microcodigos
informe informes diferido diferida diferidos diferidas
captura capturas capturar capturado demanda demandas demandar
prueba pruebas probar probado pasada pasadas pasar pasado
objeto objetos lista listas mapa mapas datos dato bloque bloques trozo trozos
ventana ventanas hilo hilos tiempo tiempos ms
codigo codigos estado estados nombre nombres numero numeros cantidad cantidades
grupo grupos conjunto conjuntos elemento elementos parametro parametros
argumento argumentos resultado resultados retorno retornos devolver devuelve
fallo fallos fallido fallida fallar exito correcto correcta correctos
valido valida validos validas invalido invalida pendiente pendientes
cambio cambios cambiar cambiado destruir destruccion destruido eliminar eliminado
borrar borrado liberar liberado liberacion libre recurso recursos
referencia referencias contenido contenidos contenedor contenedores
titulo titulos etiqueta etiquetas fecha fechas hora horas reloj segundo segundos
frecuencia vez veces fila filas columna columnas celda celdas
cadena cadenas palabra palabras letra letras unidad unidades memoria memorias
direccion direcciones puntero punteros desplazamiento desplazamientos
posicion posiciones coordenada coordenadas entero enteros flotante flotantes
decimal decimales signo bandera banderas mascara mascaras
llamada llamadas llamar llamado funcion funciones metodo metodos clase clases
tipo tipos forma formas ahora antes despues siempre nunca solo sin dentro sobre
bajo baja bajos bajas alta altas grande grandes pequeno pequena pequenos pequenas
maximo maxima maximos maximas minimo minima minimos minimas
inicio inicial iniciar iniciado inicializar inicializado
fin finalizar final finales terminar terminado
cerrar cerrado cerrada abrir abierto abierta activar activo activa activado activada
desactivar desactivado habilitar habilitado deshabilitar
establecer establecido establece configurar configuracion configurado
aplicar aplicado aplica ejecutar ejecutado procesar procesado procesamiento
calcular calculo calculado calcula mostrar mostrado mostrando mover movimiento movido
rotar rotacion rotado escala escalas traslado trasladar matriz matrices
vector vectores transparencia transparencias transparente transparentes opacidad brillo
iluminacion iluminar iluminado iluminada luz luces difusa difuso especular ambiental
niebla nieblas terreno terrenos mundo mundos coche coches carretera carreteras
pista pistas avion aviones piloto pilotos soldado soldados enemigo enemigos
jugador jugadores vida vidas salud arma armas municion municiones bala balas
mision misiones menu menus pausa juego juegos partida guardar guardado
sonido sonidos musica pelicula peliculas imagen imagenes superficie superficies
fondo fondos pantalla pantallas punto puntos linea lineas corte cortes
fijo fija fijos fijas vista vistas camara camaras escena escenas pieza piezas
parte partes nivel niveles modo modos filtro filtros fuerza fuerzas
mismo misma mismos mismas todo todos toda todas nulo nula nulos nulas
archivo archivos cuadro cuadros marco marcos origen origenes fuente fuentes
ruta rutas caja cajas carpeta carpetas nota notas cuerpo cuerpos
lado lados cara caras borde bordes centro centros rango rangos paso pasos
limite limites caso casos ciclo ciclos turno turnos suma sumar resta restar
nube nubes arbol arboles bosque bosques hierba hierbas planta plantas agua aguas
piedra piedras pared paredes puerta puertas suelo suelos cabeza cabezas
ruido ruidos eco ecos modelo modelos regla reglas flujo flujos canal canales
salto saltos retraso retrasos sincronizar sincronizacion sincronizado
bloqueo bloqueos barrera barreras cerrojo cerrojos evento eventos vaciar vaciado
vacio vacia espacio espacios eje ejes transferencia transferencias transferir
transferido pedido pedidos pedir solicitud solicitudes solicitar solicitado
mensaje mensajes aviso avisos advertencia advertencias depuracion depurar depurado
diagnostico diagnosticos estadistica estadisticas perfil perfiles envio envios
enviar enviado recepcion recibir recibido recibida accion acciones hoja hojas
triangulo triangulos poligono poligonos cuadrado cuadrados proyeccion proyectar
perspectiva bufer bufers cabecera cabeceras volcado volcar mosaico cuadricula
lienzo pincel relleno rellenar plano planos polvo humo fuego llama explosion
particula particulas lluvia nieve neblina bruma celeste tono tonos oscuro oscura
claro clara oscuridad negro blanco rojo verde azul escalonado parpadeo
"""
SPANISH_WORDS += """
abajo abiertas aciertos acuerdos adjunto adjuntos afectada agotadas ahorrados ahorro alguna alineacion alineado alinear alternancia anadir anotada anotado anotar anuladas anulados apagada apagado apagar aparte aplazada aplazadas aplazados aplazamiento aplazamientos aplazar aplicable aplicados aplicando apuntadas apuntado aqui arrancar arriba aspecto atajo atascado atributo atributos avance avanzar avisada avisado avisados avisar ayudadas ayudados ayudando ayudar banda bandas barato barreras biblioteca bien bilineal borrados borrara booleanos buscar busqueda cacheadas cacheado caducada caducadas caducados calculada caliente cambiadas campo candidata candidatas candidato candidatos canonica canonicas canonizar capas capacidad capaz carga cargada cargar cargas casilla casillas categoria categorias causa causas cercano cero coherencia coinciden coincidencias colisiones comando comandos comienzo compara comparacion comparar compartidas compensa completa completadas completar completo completos comprobables comprobacion comprobaciones comprobada comprobadas comprobados comprobando comprobar compuesta condicion conocidos conserva conservados consulta consultas contados contenedora contraste convertidos copiadas cortar coste creaciones cronometrados cronometrar cronometro cruda crudo cuadrilateros cuantas cuanto cuantos cubeta cubetas cubo cubos dedicada defecto deja delante derecha desacuerdo desalineados descarta descartadas descartar desconocidos descriptores desde detalle detalles detras diagnosticados diferencia diferencias diferir dinamico dinamicos directa directo discrepancias dispositivo disponible disponibles distinta distintas distinto distintos domo donde dorso duracion durmiendo ecuacion efectiva efectos ejecutor elegidas elegido elegir emergencia empaquetar empezado empezados empezar empieza encendido encoger encoladas encolados encolar encontrado encuadre encuadres enlace enlaces enlazada enlazado enlazados enlazar entera entorno entrante epoca escalado escalar escenario escritas escritor escritos especial especializacion espejo estencil estructura etapa etapas exacta exacto exclusivo exigir exponente exportes expulsion exterior extremo factores fallados fallidos faltan familia familias fase fases fichero fijado fingida firma firmas fisica fisico fisicos forzar foto fotos fragmentos frente fria frias frontal frontales fuera geometria girar grabacion grabaciones grabada grabadas grabado grabados grabando grabar graduacion guardia hecha hechas hecho hechos histograma holgado holgura hueco huecos identidad identificados identificar ignorar igual iguales incoherente incoherentes indexados indirectos iniciador iniciales inmediata inmediato instantanea instantaneas instruccion instrucciones intentar intento intercambiar intercambio intercambios interna interno interrupcion interrupciones intervalo intervalos inutiles inversa inverso invitado izquierda lector lectores leida leidas leidos lejos lento lentos libres limpios listos llamante llenas longitud lote magia malo malos mapeado mapeo marca marcada marcadas marcador marcadores marcar marcas mayor medida medidas medido medir mejor mejora mejoras menor menores menos miembros migraciones mira miradas mirados mirando mirar modulo modulos monton montones mostradas motivo motivos muerta muertas multiplo nada necesarios neutro nucleo objetivo oculto ocultos ocupada ocupadas ocupado olvidados olvidar olvidos omitir operacion ordenado ordenes otra otras otro otros parche parcheado parches pareja pares pase pases pedidas peor peores perdida perdidas perdidos perezosa periodo permanente permitir permutacion pesos peticion pintadas pintado pixeles podado poner posproceso preajuste preajustes precalentado precalentar precarga precargar precisa precisas precisos preferencia preparada preparadas preparador preparados preparar presentaciones presentador prestadas prestamos presupuesto previas previos prioridad productor pronto propiedades propio publicado publicados publicar puede puesta puesto puntual quedan quitadas quitar raiz rapido rapidos raras rastro recoger recogidos recogiendo recortadas rectangulo reduccion reducciones refrescados refrescos regiones registrados rehechas reiniciar reinicio relajado relevo relojes remapeo remapeos renderizador reparto repetidos representante repuesto requisitos reserva reservada reservados reservar resolucion restauraciones restaurados restaurar resto resubidas resuelta resueltas resumen retenida retirados retirar reutilizar revisadas revisar saltadas saltados saltar saltaria saturacion saturado secuencia seguidos semilla senal sesgo sesgos sinc sitio sobran solapes soltadas soltar sondeo subetapa subida subidas subidos subir subpase sucios sustituidas tabla tam tardias tardio tardios techo temprana temprano terminada testigo tijera tinte toca tomadas tomar tope topologia trabajo trabajos tramo tramos tratamiento traza trazas trazando ubicacion ubicaciones umbral usadas usado usados usar uso usos usuario util vacias validada validadas validar variante variantes varios verificados verificar vias vibracion vigilada vigiladas vigilar vineta visibilidad visibles visto vistos volumen vtabla vuelo vuelta vueltas zancada zancadas formato formatos srgb tamanos soportado anulado antes despues escala escrituras
"""
SPANISH = frozenset(SPANISH_WORDS.split())

IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
PP_NUMBER = re.compile(r"\.?[0-9](?:[eEpP][+-]|[A-Za-z0-9_.'])*")
LITERAL_PREFIXES = frozenset({"L", "u", "U", "u8", "R", "LR", "uR", "UR", "u8R"})
ALL_CAPS = re.compile(r"[A-Z0-9_]*[A-Z][A-Z0-9_]*")
CAMEL_SPLIT = re.compile(r"[A-Z]{2,}(?![a-z])|[A-Z]?[a-z]+|[A-Z]|[0-9]+")

TYPE_DECL = re.compile(
    r"\b(?:struct|class|union|enum|using|typedef)\s+"
    r"(?:class\s+|struct\s+)?([A-Za-z_][A-Za-z0-9_]*)")
FUNC_CALL = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")
DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)", re.M)
CVAR_CODE = re.compile(r"REXCVAR_DEFINE\w*\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)")
CVAR_STRING = re.compile(r'REXCVAR_DEFINE\w*\s*\(\s*"([A-Za-z_][A-Za-z0-9_]*)"')


def consume_literal(src, q, raw, n):
    """q is the index of the opening quote. Returns (end_index, body)."""
    quote = src[q]
    if raw:
        p = src.find("(", q + 1)
        if p < 0:
            p = q
        delim = src[q + 1:p]
        end = ")" + delim + quote
        e = src.find(end, p + 1)
        if e < 0:
            return n, src[p + 1:n]
        return e + len(end), src[p + 1:e]
    j = q + 1
    while j < n and src[j] != quote and src[j] != "\n":
        j += 2 if src[j] == "\\" else 1
    j = min(j, n)
    body = src[q + 1:j]
    if j < n and src[j] == quote:
        j += 1
    return j, body


def scan(src):
    """Returns (code, strings, comments).
    code: source with literals and comments blanked (newlines kept).
    strings: [(line, body)] for double-quoted literals.
    comments: [(line, text)] one entry per physical comment line.
    """
    out = []
    strings = []
    comments = []
    n = len(src)
    i = 0
    line = 1

    def blank(seg):
        return re.sub(r"[^\n]", " ", seg)

    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "\n":
            out.append(c)
            line += 1
            i += 1
        elif c == "/" and nxt == "/":
            j = src.find("\n", i)
            if j < 0:
                j = n
            comments.append((line, src[i + 2:j]))
            out.append(" " * (j - i))
            i = j
        elif c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            seg = src[i:j]
            for k, part in enumerate(seg.split("\n")):
                comments.append((line + k, part))
            out.append(blank(seg))
            line += seg.count("\n")
            i = j
        elif c.isalpha() or c == "_":
            m = IDENT.match(src, i)
            word = m.group(0)
            k = m.end()
            if k < n and src[k] in "\"'" and word in LITERAL_PREFIXES:
                j, body = consume_literal(src, k, word.endswith("R"), n)
                if src[k] == '"':
                    strings.append((line, body))
                seg = src[i:j]
                out.append(blank(seg))
                line += seg.count("\n")
                i = j
            else:
                out.append(word)
                i = k
        elif c in "\"'":
            j, body = consume_literal(src, i, False, n)
            if c == '"':
                strings.append((line, body))
            seg = src[i:j]
            out.append(blank(seg))
            line += seg.count("\n")
            i = j
        elif "0" <= c <= "9" or (c == "." and nxt.isdigit()):
            m = PP_NUMBER.match(src, i)
            seg = m.group(0)
            out.append(" " * len(seg))
            i += len(seg)
        else:
            out.append(c)
            i += 1
    return "".join(out), strings, comments


def split_identifier(name):
    words = []
    for part in name.split("_"):
        if part:
            words += CAMEL_SPLIT.findall(part)
    return [w.lower() for w in words]


def natural_words(text):
    """Words of free text (strings, comments): accents removed, camel split."""
    words = []
    for tok in re.findall(r"[^\W\d_]+", text):
        t = unicodedata.normalize("NFKD", tok)
        t = "".join(ch for ch in t if not unicodedata.combining(ch))
        t = t.encode("ascii", "ignore").decode("ascii")
        words += split_identifier(t)
    return words


def spanish_hits(text):
    return sorted({w for w in natural_words(text) if w in SPANISH})


def is_all_caps(name):
    return len(name) > 1 and ALL_CAPS.fullmatch(name) is not None


def line_of_offsets(code):
    starts = [0]
    for m in re.finditer("\n", code):
        starts.append(m.end())
    return starts


def esc(text):
    return (text.replace("\\", "\\\\").replace("\t", "\\t")
            .replace("\r", "\\r").replace("\n", "\\n"))


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_ROOT
    paths = []
    for dp, _, fns in os.walk(root):
        for fn in fns:
            paths.append(os.path.join(dp, fn))
    paths.sort()

    files = {}  # rel -> dict(code, strings, comments, raw)
    for p in paths:
        with open(p, "rb") as fh:
            raw = fh.read().decode("utf-8", errors="replace")
        raw = raw.lstrip("\ufeff").replace("\r\n", "\n")
        code, strings, comments = scan(raw)
        rel = os.path.relpath(p, root).replace(os.sep, "/")
        files[rel] = {"code": code, "strings": strings,
                      "comments": comments, "raw": raw}

    # Global declaration sets (across all files).
    type_names, func_names, define_names, cvar_names = set(), set(), set(), set()
    for f in files.values():
        code = f["code"]
        type_names.update(TYPE_DECL.findall(code))
        func_names.update(FUNC_CALL.findall(code))
        define_names.update(DEFINE.findall(code))
        cvar_names.update(CVAR_CODE.findall(code))
        cvar_names.update(CVAR_STRING.findall(f["raw"]))

    occ = Counter()
    where = defaultdict(set)
    for rel, f in files.items():
        for m in IDENT.finditer(f["code"]):
            occ[m.group(0)] += 1
            where[m.group(0)].add(rel)

    def kind_of(name):
        if name in cvar_names:
            return "Cvar"
        if name in type_names and name[0].isupper() and re.search(r"[a-z]", name):
            return "Type"
        if name in define_names or is_all_caps(name):
            return "Macro"
        if name in func_names:
            return "Function"
        return "Variable"

    # identifiers.tsv
    ident_rows = []
    for name, count in occ.items():
        hits = sorted({w for w in split_identifier(name) if w in SPANISH})
        if not hits:
            continue
        ident_rows.append((name, count, sorted(where[name]), kind_of(name), hits))
    ident_rows.sort(key=lambda r: (-r[1], r[0]))

    with open(os.path.join(OUT_DIR, "identifiers.tsv"), "w",
              encoding="utf-8", newline="\n") as fh:
        fh.write("identifier\toccurrences\tfiles\tkind\tspanish_words\n")
        for name, count, fl, kind, hits in ident_rows:
            fh.write(f"{name}\t{count}\t{','.join(fl)}\t{kind}\t{'+'.join(hits)}\n")

    # strings.tsv
    string_rows = []
    for rel in sorted(files):
        for line, body in files[rel]["strings"]:
            hits = spanish_hits(body)
            if hits:
                string_rows.append((f"{rel}:{line}", "+".join(hits), esc(body)))
    with open(os.path.join(OUT_DIR, "strings.tsv"), "w",
              encoding="utf-8", newline="\n") as fh:
        fh.write("location\tspanish_words\ttext\n")
        for loc, hits, text in string_rows:
            fh.write(f"{loc}\t{hits}\t{text}\n")

    # comments_count.tsv
    comment_total = 0
    with open(os.path.join(OUT_DIR, "comments_count.tsv"), "w",
              encoding="utf-8", newline="\n") as fh:
        fh.write("file\tspanish_comment_lines\tsample_lines\n")
        for rel in sorted(files):
            merged = {}
            for line, text in files[rel]["comments"]:
                merged[line] = (merged.get(line, "") + " " + text).strip()
            hits_lines = [(ln, t) for ln, t in sorted(merged.items())
                          if spanish_hits(t)]
            comment_total += len(hits_lines)
            samples = " | ".join(
                f"L{ln}: {t[:160]}" for ln, t in hits_lines[:5])
            fh.write(f"{rel}\t{len(hits_lines)}\t{esc(samples)}\n")
        fh.write(f"TOTAL\t{comment_total}\t\n")

    # Summary
    kinds = Counter(r[3] for r in ident_rows)
    print(f"source root: {root}")
    print(f"files scanned: {len(files)}")
    print(f"Spanish identifiers: {len(ident_rows)} "
          f"(total occurrences {sum(r[1] for r in ident_rows)})")
    for k in ("Type", "Function", "Macro", "Cvar", "Variable"):
        print(f"  {k}: {kinds.get(k, 0)}")
    print(f"Spanish string literals: {len(string_rows)}")
    print(f"Spanish comment lines: {comment_total}")
    print("top 30 Spanish identifiers:")
    for name, count, fl, kind, hits in ident_rows[:30]:
        print(f"  {count:6d}  {name}  [{kind}]")
    print(f"outputs in: {OUT_DIR}")


if __name__ == "__main__":
    main()
