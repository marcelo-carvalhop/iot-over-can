# Pré-script do PlatformIO: gera src/documentos.h a partir da documentação do
# repositório antes de cada compilação (só reescreve o arquivo se o texto mudou).
import sys
from pathlib import Path

Import("env")  # noqa: F821 (fornecido pelo PlatformIO)

projeto = Path(env.subst("$PROJECT_DIR"))  # noqa: F821
sys.path.insert(0, str(projeto / "tools"))

import documentos  # noqa: E402

documentos.gerar(projeto / "src" / "documentos.h", projeto.parents[1])
