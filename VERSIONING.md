# Versionamento

O M5tabOS segue [SemVer](https://semver.org/lang/pt-BR/): `MAIOR.MENOR.CORREÇÃO`.
O número mora em `version.txt` e só muda pelo `tools/release.sh`.

## Qual número subir

Enquanto a versão começar com **0.**, o sistema ainda está tomando forma. A
regra é:

| O que a versão traz | Sobe | Exemplo |
|---|---|---|
| Só correções, sem nada novo para quem usa | CORREÇÃO (`patch`) | 0.7.0 → 0.7.1 |
| Qualquer coisa nova: app, tela, ajuste, serviço | MENOR (`minor`) | 0.7.1 → 0.8.0 |
| Algo que **exige regravar com `./flash.sh`** (tabela de partições, formato da NVS, formato de arquivo no cartão que a versão antiga não lê) | MENOR (`minor`), e o CHANGELOG avisa em negrito | 0.8.0 → 0.9.0 |
| O sistema é considerado estável e a API dos apps (`kernel/app.h`) passa a ser um compromisso | MAIOR: o **1.0.0** | 0.x → 1.0.0 |

Depois do 1.0.0, uma mudança que quebra apps já escritos, ou que exige
regravar, passa a subir o MAIOR.

Na dúvida entre dois números, use o maior. Um número a mais não custa nada.
Um a menos esconde uma mudança.

## O dia a dia

1. Toda mudança que alguém vai notar ganha uma linha em `CHANGELOG.md`, na
   seção **Não lançado**, no mesmo commit da mudança.
2. Quando o que está ali vale uma versão, rode:
   ```bash
   tools/release.sh minor
   ```
   O script mostra de qual versão para qual vai e pede confirmação. Depois ele
   grava o `version.txt`, transforma o "Não lançado" na seção da versão, com a
   data, e cria o commit `Versao X.Y.Z` e a tag `vX.Y.Z`.
3. Publique: `git push origin main vX.Y.Z`.

## O que garante que a versão só anda para a frente

- O `release.sh` recusa um número que não seja maior que a última tag, recusa
  lançar com mudanças não commitadas e recusa lançar com o "Não lançado" vazio.
- O build (`CMakeLists.txt`) **para com erro** se o `version.txt` estiver
  abaixo da última versão lançada.
- O número gravado no binário diz de onde o build saiu:
  - `0.8.0`: exatamente a versão lançada;
  - `0.8.0+12.a1b2c3d`: 12 commits depois dela, no commit `a1b2c3d`.

  É esse número que aparece em Ajustes, no console (`estado`) e no log de
  boot. Assim, um aparelho com um build de desenvolvimento nunca se passa por
  uma versão lançada. Mudanças ainda não commitadas não aparecem no número:
  `+N.hash` quer dizer "construído a partir de".
