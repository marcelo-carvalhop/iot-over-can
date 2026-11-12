"""Identificação do operador que usa a estação de supervisão.

O firmware não sabe quem está do outro lado da serial: cada comando chega como
uma linha de texto. Quem sabe é a TUI, e é nela que a identificação acontece.
Este módulo guarda o cadastro de operadores e controla a sessão de quem está
operando. O registro no histórico fica em ``audit.py`` e em ``history/``.

Em uma frase: o operador se identifica com nome e senha, a sessão expira por
inatividade e tudo que a TUI envia à rede em nome dele é gravado no histórico
com o nome dele.

Três modos de operação:

* ``auto`` (padrão): se existe cadastro com operadores, o login passa a ser
  exigido para alterar a rede; sem cadastro, o responsável é o usuário do
  sistema operacional da estação (nada muda para quem não configurou nada,
  mas o histórico já sabe quem estava na estação);
* ``required``: o login é sempre exigido; sem operador cadastrado, nada que
  altere a rede é enviado;
* ``off``: sem identificação e sem registro de ações (comportamento antigo,
  para ensaios automatizados).

O que isto garante e o que não garante está em ``Documentacao/seguranca.md``.
Em resumo: separa quem fez o quê entre pessoas que dividem a bancada e deixa um
rastro verificável. Não resiste a quem tem acesso de administrador à estação.
A chave FIDO2, quando existir, entra no mesmo ponto: ``Actor.method``.
"""
from __future__ import annotations

import base64
import binascii
import contextlib
import functools
import getpass
import hashlib
import hmac
import json
import os
import re
import secrets
import socket
import tempfile
import time
import unicodedata
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path

CONFIG_DIR_ENV = "IOT_OVER_CAN_CONFIG_DIR"
REGISTRY_VERSION = 1

ROLE_OPERATOR = "operator"
ROLE_OBSERVER = "observer"
ROLES = (ROLE_OPERATOR, ROLE_OBSERVER)
ROLE_LABELS = {ROLE_OPERATOR: "operador", ROLE_OBSERVER: "observador"}

MODE_AUTO = "auto"
MODE_REQUIRED = "required"
MODE_OFF = "off"
MODES = (MODE_AUTO, MODE_REQUIRED, MODE_OFF)

METHOD_PASSWORD = "password"
METHOD_STATION = "station"

#: Prefixo do identificador de quem foi reconhecido só pelo sistema operacional.
#: Nomes cadastrados não contêm ":", então os dois nunca se confundem.
STATION_PREFIX = "sys:"

MIN_PASSWORD_LENGTH = 8
MAX_PASSWORD_LENGTH = 256
DEFAULT_IDLE_TIMEOUT_S = 900.0
DEFAULT_COST = (2**14, 8, 1)  # N, r, p do scrypt (cerca de 16 MiB e 50 ms)
MAX_SCRYPT_BYTES = 256 * 1024 * 1024

NAME_RE = re.compile(r"^[a-z][a-z0-9._-]{1,31}$")
_CONTROL_CHARS = re.compile(r"[\x00-\x1f\x7f]")


class OperatorError(ValueError):
    """Dado de operador inválido (nome, senha, papel) ou cadastro ilegível."""


# ---------------------------------------------------------------------------
# Arquivos e senhas
# ---------------------------------------------------------------------------


def config_dir() -> Path:
    override = os.environ.get(CONFIG_DIR_ENV, "").strip()
    return Path(override) if override else Path.home() / ".config" / "iot-over-can"


def default_registry_path() -> Path:
    return config_dir() / "operators.json"


def normalize_name(name: str) -> str:
    return str(name or "").strip().lower()


def validate_name(name: str) -> str:
    name = normalize_name(name)
    if not NAME_RE.fullmatch(name):
        raise OperatorError(
            "Nome inválido: use de 2 a 32 caracteres, começando por letra minúscula, "
            "com letras, números, ponto, hífen ou sublinhado."
        )
    return name


def clean_display(display: str) -> str:
    return _CONTROL_CHARS.sub(" ", str(display or "")).strip()[:64]


def validate_password(password: str, name: str = "") -> None:
    if not isinstance(password, str) or len(password) < MIN_PASSWORD_LENGTH:
        raise OperatorError(f"A senha precisa de pelo menos {MIN_PASSWORD_LENGTH} caracteres.")
    if len(password) > MAX_PASSWORD_LENGTH:
        raise OperatorError(f"A senha pode ter no máximo {MAX_PASSWORD_LENGTH} caracteres.")
    if name and password.strip().lower() == normalize_name(name):
        raise OperatorError("A senha não pode ser igual ao nome do operador.")


def _b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def _unb64(text: object) -> bytes:
    return base64.b64decode(str(text), validate=True)


def _cost_ok(n: int, r: int, p: int) -> bool:
    return (
        16 <= n <= 2**20
        and n & (n - 1) == 0
        and 1 <= r <= 32
        and 1 <= p <= 16
        and 128 * r * (n + p + 2) <= MAX_SCRYPT_BYTES
    )


def _derive(password: str, salt: bytes, n: int, r: int, p: int) -> bytes:
    secret = unicodedata.normalize("NFKC", password).encode("utf-8")
    return hashlib.scrypt(secret, salt=salt, n=n, r=r, p=p, dklen=32, maxmem=128 * r * (n + p + 2) + (1 << 20))


def hash_password(password: str, cost: tuple[int, int, int] = DEFAULT_COST) -> dict[str, object]:
    """Credencial para guardar no cadastro: nunca a senha, só o resultado do scrypt."""

    n, r, p = cost
    if not _cost_ok(n, r, p):
        raise OperatorError("Parâmetros do scrypt fora dos limites aceitos.")
    salt = secrets.token_bytes(16)
    return {"kdf": "scrypt", "n": n, "r": r, "p": p, "salt": _b64(salt), "hash": _b64(_derive(password, salt, n, r, p))}


def verify_password(password: str, credential: Mapping[str, object]) -> bool:
    try:
        n, r, p = int(credential["n"]), int(credential["r"]), int(credential["p"])  # type: ignore[call-overload]
        salt, expected = _unb64(credential["salt"]), _unb64(credential["hash"])
    except (KeyError, TypeError, ValueError, binascii.Error):
        return False
    if credential.get("kdf") != "scrypt" or not _cost_ok(n, r, p) or len(salt) < 8 or len(expected) < 16:
        return False
    try:
        derived = _derive(str(password), salt, n, r, p)
    except (ValueError, MemoryError):
        return False
    return len(derived) == len(expected) and hmac.compare_digest(derived, expected)


@functools.lru_cache(maxsize=1)
def _dummy_credential() -> dict[str, object]:
    return hash_password("credencial-descartavel")


def _now_iso() -> str:
    return datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ")


# ---------------------------------------------------------------------------
# Cadastro
# ---------------------------------------------------------------------------


@dataclass(slots=True)
class OperatorRecord:
    name: str
    role: str = ROLE_OPERATOR
    display: str = ""
    enabled: bool = True
    #: Sobe a cada troca de senha, desativação ou mudança de papel: a sessão
    #: aberta antes da mudança deixa de valer.
    serial: int = 1
    created: str = ""
    credential: dict[str, object] = field(default_factory=dict)

    def to_json(self) -> dict[str, object]:
        return {
            "name": self.name, "display": self.display, "role": self.role, "enabled": self.enabled,
            "serial": self.serial, "created": self.created, **self.credential,
        }

    @classmethod
    def from_json(cls, raw: object) -> OperatorRecord:
        if not isinstance(raw, dict):
            raise OperatorError("cada operador deve ser um objeto JSON")
        name = validate_name(str(raw.get("name", "")))
        role = str(raw.get("role", ROLE_OPERATOR))
        if role not in ROLES:
            raise OperatorError(f"operador {name}: papel desconhecido ({role})")
        credential = {key: raw[key] for key in ("kdf", "n", "r", "p", "salt", "hash") if key in raw}
        if credential.get("kdf") != "scrypt" or "salt" not in credential or "hash" not in credential:
            raise OperatorError(f"operador {name}: credencial ausente ou de tipo desconhecido")
        serial = raw.get("serial", 1)
        return cls(
            name=name, role=role, display=clean_display(str(raw.get("display", ""))),
            enabled=bool(raw.get("enabled", True)), serial=serial if isinstance(serial, int) and serial > 0 else 1,
            created=str(raw.get("created", "")), credential=credential,
        )


class OperatorRegistry:
    """Arquivo ``operators.json``: nomes, papéis e senhas em hash (permissão 0600).

    Como o ``security.json``, o arquivo é recusado se pertencer a outro usuário
    ou se outros usuários puderem ler: erro de configuração bloqueia, não libera.
    """

    def __init__(self, path: str | Path | None = None, *, cost: tuple[int, int, int] = DEFAULT_COST) -> None:
        self.path = Path(path) if path else default_registry_path()
        self.cost = cost
        self.error = ""
        self._records: dict[str, OperatorRecord] = {}
        self._stamp: tuple[int, int, int] | None = None
        self.reload()

    # -- leitura ----------------------------------------------------------

    def _signature(self) -> tuple[int, int, int] | None:
        try:
            st = self.path.stat()
        except OSError:
            return None
        return (st.st_mtime_ns, st.st_size, st.st_ino)

    def reload(self) -> None:
        self.error = ""
        self._records = {}
        self._stamp = self._signature()
        if self._stamp is None:
            return
        try:
            st = self.path.stat()
            if os.name == "posix":
                if hasattr(os, "getuid") and st.st_uid != os.getuid():
                    raise PermissionError("operators.json não pertence ao usuário atual")
                if st.st_mode & 0o077:
                    raise PermissionError("operators.json deve usar permissões 0600")
            data = json.loads(self.path.read_text(encoding="utf-8"))
            if not isinstance(data, dict) or data.get("version") != REGISTRY_VERSION or not isinstance(data.get("operators"), list):
                raise OperatorError("operators.json fora do formato esperado")
            records: dict[str, OperatorRecord] = {}
            for raw in data["operators"]:
                record = OperatorRecord.from_json(raw)
                if record.name in records:
                    raise OperatorError(f"operador repetido: {record.name}")
                records[record.name] = record
        except (OSError, ValueError) as exc:  # JSONDecodeError e OperatorError são ValueError
            self.error = str(exc)
            return
        self._records = records

    def refresh(self) -> bool:
        """Relê o arquivo se ele mudou desde a última leitura (a CLI pode editá-lo)."""

        if self._signature() != self._stamp:
            self.reload()
            return True
        return False

    @property
    def exists(self) -> bool:
        return self._stamp is not None

    def records(self) -> list[OperatorRecord]:
        return sorted(self._records.values(), key=lambda item: item.name)

    def get(self, name: str) -> OperatorRecord | None:
        return self._records.get(normalize_name(name))

    def active_count(self) -> int:
        return sum(1 for record in self._records.values() if record.enabled)

    def verify(self, name: str, password: str) -> OperatorRecord | None:
        """Devolve o operador se nome e senha conferem. Gasta o mesmo tempo nos dois casos."""

        record = self._records.get(normalize_name(name))
        if record is None or not record.enabled:
            verify_password(str(password), _dummy_credential())
            return None
        return record if verify_password(str(password), record.credential) else None

    # -- escrita (usada pela CLI) -----------------------------------------

    def _writable(self) -> None:
        self.reload()
        if self.error:
            raise OperatorError(f"Cadastro ilegível, nada foi alterado: {self.error}")

    def _require(self, name: str) -> OperatorRecord:
        record = self.get(name)
        if record is None:
            raise OperatorError(f"Operador não encontrado: {normalize_name(name)}")
        return record

    def add(self, name: str, password: str, *, role: str = ROLE_OPERATOR, display: str = "") -> OperatorRecord:
        name = validate_name(name)
        if role not in ROLES:
            raise OperatorError(f"Papel inválido: {role} (use {' ou '.join(ROLES)})")
        validate_password(password, name)
        self._writable()
        if name in self._records:
            raise OperatorError(f"Operador já cadastrado: {name}")
        record = OperatorRecord(
            name=name, role=role, display=clean_display(display), created=_now_iso(),
            credential=hash_password(password, self.cost),
        )
        self._records[name] = record
        self._save()
        return record

    def set_password(self, name: str, password: str) -> OperatorRecord:
        self._writable()
        record = self._require(name)
        validate_password(password, record.name)
        record.credential = hash_password(password, self.cost)
        record.serial += 1
        self._save()
        return record

    def set_enabled(self, name: str, enabled: bool) -> OperatorRecord:
        self._writable()
        record = self._require(name)
        if record.enabled != enabled:
            record.enabled = enabled
            record.serial += 1
            self._save()
        return record

    def set_role(self, name: str, role: str) -> OperatorRecord:
        if role not in ROLES:
            raise OperatorError(f"Papel inválido: {role} (use {' ou '.join(ROLES)})")
        self._writable()
        record = self._require(name)
        if record.role != role:
            record.role = role
            record.serial += 1
            self._save()
        return record

    def remove(self, name: str) -> None:
        self._writable()
        record = self._require(name)
        del self._records[record.name]
        self._save()

    def _save(self) -> None:
        payload = {"version": REGISTRY_VERSION, "operators": [record.to_json() for record in self.records()]}
        text = json.dumps(payload, ensure_ascii=False, indent=2) + "\n"
        directory = self.path.parent
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(prefix=".operators-", suffix=".tmp", dir=directory)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as handle:
                handle.write(text)
                handle.flush()
                os.fsync(handle.fileno())
            os.chmod(tmp, 0o600)
            os.replace(tmp, self.path)
        except BaseException:
            with contextlib.suppress(OSError):
                os.unlink(tmp)
            raise
        self._stamp = self._signature()


# ---------------------------------------------------------------------------
# Sessão
# ---------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class Actor:
    """Quem responde por um comando: o que vai para o histórico e para os registros."""

    name: str
    role: str = ROLE_OPERATOR
    method: str = METHOD_PASSWORD
    display: str = ""
    station: str = ""
    serial: int = 0

    @property
    def id(self) -> str:
        return self.name if self.method == METHOD_PASSWORD else f"{STATION_PREFIX}{self.name}"

    @property
    def verified(self) -> bool:
        """Só a senha comprova quem é; o usuário do sistema é apenas declarado."""

        return self.method == METHOD_PASSWORD

    @property
    def can_modify(self) -> bool:
        return self.role == ROLE_OPERATOR

    def label(self) -> str:
        who = self.display or self.name
        return who if self.verified else f"{who} (sistema)"

    def record(self) -> dict[str, object]:
        return {
            "id": self.id, "name": self.name, "display": self.display, "role": self.role,
            "method": self.method, "verified": self.verified, "station": self.station,
        }


@dataclass(frozen=True, slots=True)
class LoginResult:
    ok: bool
    actor: Actor | None = None
    reason: str = ""
    retry_after_s: float = 0.0


@dataclass(frozen=True, slots=True)
class OperatorDecision:
    allowed: bool
    actor: Actor | None = None
    reason: str = ""
    #: "", "login_required", "read_only", "registry_error" ou "no_operators".
    code: str = ""


def station_actor(user: str | None = None, host: str | None = None) -> Actor:
    if not user:
        try:
            user = getpass.getuser()
        except (KeyError, OSError, ImportError):  # sem entrada no passwd nem variáveis de ambiente
            user = os.environ.get("USER") or os.environ.get("USERNAME") or "desconhecido"
    host = host or socket.gethostname()
    return Actor(name=user, role=ROLE_OPERATOR, method=METHOD_STATION, station=f"{user}@{host}")


class OperatorSession:
    """Quem está operando a estação agora, e se pode alterar a rede."""

    def __init__(
        self,
        registry: OperatorRegistry | None = None,
        *,
        mode: str = MODE_AUTO,
        idle_timeout_s: float = DEFAULT_IDLE_TIMEOUT_S,
        clock: Callable[[], float] = time.monotonic,
        station: Actor | None = None,
        max_failures: int = 5,
        lockout_s: float = 30.0,
    ) -> None:
        mode = (mode or MODE_AUTO).lower()
        self.mode = mode if mode in MODES else MODE_AUTO
        # No modo desligado o cadastro nem é aberto: nenhum arquivo da pessoa é lido.
        self.registry = registry if registry is not None else (OperatorRegistry() if self.mode != MODE_OFF else None)
        self.idle_timeout_s = max(0.0, float(idle_timeout_s))
        self._clock = clock
        self._station = station or station_actor()
        self.max_failures = max_failures
        self.lockout_s = lockout_s
        self._actor: Actor | None = None
        self._last_activity = 0.0
        self._failures = 0
        self._lockouts = 0
        self._locked_until = 0.0

    # -- situação ---------------------------------------------------------

    @property
    def kind(self) -> str:
        """``off`` (sem identificação), ``station`` (usuário do sistema) ou ``login`` (senha)."""

        if self.mode == MODE_OFF or self.registry is None:
            return "off"
        if self.mode == MODE_REQUIRED:
            return "login"
        self.registry.refresh()
        return "login" if self.registry.error or self.registry.records() else "station"

    @property
    def station_actor(self) -> Actor:
        return self._station

    def _end_reason(self, now: float) -> str | None:
        actor = self._actor
        if actor is None or self.registry is None:
            return None
        if self.idle_timeout_s > 0 and now - self._last_activity >= self.idle_timeout_s:
            return "idle"
        self.registry.refresh()
        record = None if self.registry.error else self.registry.get(actor.name)
        if record is None or not record.enabled or record.serial != actor.serial:
            return "revoked"
        return None

    def current(self) -> Actor | None:
        """Quem responde pelos comandos agora; ``None`` se ninguém está identificado."""

        kind = self.kind
        if kind == "off":
            return None
        if kind == "station":
            return self._station
        if self._actor is not None and self._end_reason(self._clock()) is None:
            return self._actor
        return None

    def poll(self) -> tuple[Actor, str] | None:
        """Encerra a sessão vencida e devolve (quem, motivo) uma única vez.

        Motivos: ``idle`` (inatividade) e ``revoked`` (a senha foi trocada, o
        operador foi desativado ou removido, ou o cadastro ficou ilegível).
        """

        actor = self._actor
        if actor is None:
            return None
        reason = self._end_reason(self._clock())
        if reason is None:
            return None
        self._actor = None
        return actor, reason

    def touch(self) -> None:
        """Teclado, mouse ou comando: renova a sessão, mas só enquanto ela ainda vale.

        Uma sessão que já passou do prazo não é revivida por uma tecla apertada
        antes de ``poll()`` encerrá-la: só um novo login a renova.
        """

        if self._actor is None:
            return
        now = self._clock()
        if self.idle_timeout_s > 0 and now - self._last_activity >= self.idle_timeout_s:
            return
        self._last_activity = now

    def seconds_left(self) -> float | None:
        if self._actor is None or self.idle_timeout_s <= 0:
            return None
        return max(0.0, self.idle_timeout_s - (self._clock() - self._last_activity))

    # -- entrada e saída --------------------------------------------------

    def login(self, name: str, password: str) -> LoginResult:
        if self.registry is None:
            return LoginResult(False, reason="A identificação por senha está desligada.")
        now = self._clock()
        if self._locked_until > now:
            wait = self._locked_until - now
            return LoginResult(False, reason=f"Muitas tentativas. Aguarde {wait:.0f} s.", retry_after_s=wait)
        self.registry.refresh()
        if self.registry.error:
            return LoginResult(False, reason=f"Cadastro de operadores inválido: {self.registry.error}")
        if not self.registry.active_count():
            return LoginResult(False, reason="Nenhum operador ativo no cadastro. Use iot-over-can-operators add NOME.")
        record = self.registry.verify(name, password)
        if record is None:
            self._failures += 1
            if self._failures >= self.max_failures:
                self._failures = 0
                self._lockouts += 1
                wait = min(900.0, self.lockout_s * 2 ** (self._lockouts - 1))
                self._locked_until = now + wait
                return LoginResult(False, reason=f"Muitas tentativas. Aguarde {wait:.0f} s.", retry_after_s=wait)
            return LoginResult(False, reason="Nome ou senha incorretos.")
        self._failures = 0
        self._lockouts = 0
        self._actor = Actor(
            name=record.name, role=record.role, method=METHOD_PASSWORD, display=record.display,
            station=self._station.station, serial=record.serial,
        )
        self._last_activity = now
        return LoginResult(True, self._actor)

    def logout(self) -> Actor | None:
        actor, self._actor = self._actor, None
        return actor

    # -- decisão ----------------------------------------------------------

    def authorize(self, *, mutating: bool) -> OperatorDecision:
        """Pode quem está na estação enviar este tipo de comando?"""

        kind = self.kind
        if kind == "off":
            return OperatorDecision(True)
        if kind == "station":
            return OperatorDecision(True, self._station)
        actor = self.current()
        if not mutating:
            return OperatorDecision(True, actor)
        assert self.registry is not None
        if self.registry.error:
            return OperatorDecision(False, actor, f"Cadastro de operadores inválido: {self.registry.error}", "registry_error")
        if actor is None:
            if not self.registry.active_count():
                return OperatorDecision(False, None, "Nenhum operador cadastrado. Use iot-over-can-operators add NOME.", "no_operators")
            return OperatorDecision(False, None, "Identifique-se para alterar a rede: tecle o ou use :login.", "login_required")
        if not actor.can_modify:
            return OperatorDecision(False, actor, f"{actor.label()} tem o perfil de observador e não altera a rede.", "read_only")
        return OperatorDecision(True, actor)

    # -- apresentação -----------------------------------------------------

    def label(self) -> str:
        """Texto curto para a faixa de status."""

        kind = self.kind
        if kind == "off":
            return ""
        actor = self.current()
        if kind == "station":
            return self._station.label()
        if actor is None:
            return "sem login"
        role = "" if actor.can_modify else " (observador)"
        return f"{actor.label()}{role}"

    def describe(self) -> list[str]:
        """Linhas para o comando :operador."""

        kind = self.kind
        if kind == "off":
            return ["Identificação do operador: desligada."]
        if kind == "station":
            return [
                f"Responsável pelos comandos: {self._station.station} (usuário do sistema, não verificado).",
                "Para identificar cada pessoa por senha: iot-over-can-operators add NOME",
            ]
        actor = self.current()
        if actor is None:
            return ["Ninguém identificado. Tecle o (ou :login) para entrar."]
        lines = [f"Operador: {actor.label()}  perfil: {ROLE_LABELS.get(actor.role, actor.role)}"]
        left = self.seconds_left()
        if left is not None:
            lines.append(f"A sessão expira por inatividade em {left / 60:.0f} min.")
        return lines
