# Comissionamento de sensores sem compilação (proposta)

**Não implementado.** Este documento registra a estratégia escolhida para que a chave do enlace chegue ao sensor pela rede, depois da aprovação do operador, sem compilar um firmware por sensor. As decisões em aberto estão na seção 7.

## 1. Problema

Hoje a chave do enlace entra no binário do sensor (`build_pico.sh`, com a chave mestra ou com as chaves derivadas para o UUID dele) e no binário dos Nodes (`upload_esp32_can_node.sh`, a partir de `.env.local`). Cada sensor novo exige compilar e gravar um firmware próprio. Em um produto, o operador não deveria compilar nada: todos os sensores sairiam da fábrica com o mesmo firmware e receberiam a chave na instalação.

## 2. O que a aprovação precisa provar

O UUID que aparece na TUI é público: o sensor o anuncia por BLE e qualquer aparelho pode anunciar o mesmo número. Uma aprovação que se limitasse a escolher o UUID na tela permitiria que alguém ao alcance, no momento da instalação, se passasse pelo sensor e recebesse a chave. A aprovação precisa provar que o operador tem o sensor em mãos.

## 3. Estratégia: código de instalação

Mesmo princípio do *install code* do Zigbee 3.0 e do código de configuração do Matter.

1. **Fábrica, uma vez, sem compilar.** Na primeira partida o sensor gera um código aleatório de 128 bits (`IC`), grava na flash e o informa por um comando no console USB. O código é impresso em uma etiqueta ou QR colado no sensor. O firmware é o mesmo para todos.
2. **Instalação.** Na tela Sensores sem fio, o sensor ainda não comissionado aparece como tal. O operador o escolhe, digita ou lê o código da etiqueta, escolhe o Node e a política de reassociação.
3. **Montagem do pacote, na estação.** A TUI deriva a chave do sensor da mesma forma que os Nodes fazem hoje e a protege com o código de instalação:

   ```text
   K_dev   = HMAC(K_master, "ioc-dev-v1" || UUID)
   K_net   = HMAC(K_master, "ioc-net-v1")
   K_comm  = HMAC(IC, "ioc-commission-v1" || UUID)
   pacote  = versão || UUID || nonce || cifra(K_comm, K_dev || K_net) || tag(K_comm)
   ```

   A cifra é um fluxo derivado de HMAC-SHA256 (modo contador) e a tag é um HMAC truncado sobre todo o pacote, cifrar e depois autenticar. Só usa o que a biblioteca `ioc_link` já tem.
4. **Entrega.** O pacote vai da TUI à Probe 00 pela serial, ao Node pelo transporte segmentado do CAN e do Node ao sensor por anúncios BLE, em alguns pedaços que se alternam com a oferta normal. O sensor, que já procura ofertas, junta os pedaços, confere a tag, decifra e grava `K_dev` e `K_net` na flash. A partir daí o caminho é o que funcionou na bancada de 08/10/2026: oferta, Wi-Fi, HELLO, sessão.

O código de instalação não trafega. No CAN e no ar só passa o pacote cifrado, inútil para quem não tem a etiqueta. Com 128 bits aleatórios, não há como descobrir o código por tentativa a partir do pacote capturado.

A entrega por anúncios BLE evita uma conexão GATT e não exige trocar a configuração do ponto de acesso do Node, que pode estar servindo outros sensores.

## 4. Nodes e estação

Os Nodes continuam derivando a chave de cada sensor a partir da chave mestra. Assim qualquer Node atende qualquer sensor e a reassociação automática não depende da estação ligada. A chave mestra deixaria de ser compilada: seria gravada uma vez na NVS do Node por um comando na USB, na instalação.

A estação passa a ser a guardiã da chave mestra (como o *trust center* do Zigbee). Ela já a guarda hoje em `.env.local`; a TUI passaria a lê-la de um arquivo próprio, com permissão 600.

## 5. Troca de chave (segunda etapa)

Com a sessão aberta, a estação poderia enviar ao sensor uma nova geração da chave, protegida pela sessão em vigor (`K_dev` passaria a incluir um número de geração, levado também no vínculo para que os Nodes derivem a mesma). Útil para revogar um sensor sem trocar a chave mestra da instalação.

## 6. Limites

- **Flash legível no Pico W.** No RP2040 não há área protegida: quem tiver o sensor em mãos lê a flash e obtém `K_dev` e `K_net`. O Pico 2 W (RP2350) tem memória OTP e partida segura, que permitem guardar a chave fora do alcance de quem regrava o firmware. É o caminho para um produto.
- **Chave mestra nos Nodes.** Quem extrair a chave mestra de um Node obtém a chave de todos os sensores. É o preço da reassociação sem a estação.
- **Barramento sem autenticação.** O pedido de comissionamento no CAN não é autenticado (ver `modelo_de_seguranca.md`). Um pedido forjado não entrega chave a ninguém, porque o pacote só é útil a quem tem o código de instalação.

## 7. Decisões em aberto

| Decisão | Proposta |
|---|---|
| Como a aprovação prova a posse do sensor | código de instalação de 128 bits na etiqueta |
| Onde fica a chave mestra | nos Nodes, gravada pela USB, sem compilação |
| Escopo da primeira etapa | comissionamento (seção 3); troca de chave depois |
| Branch | novo, a partir da versão validada em bancada |

### Alternativas consideradas

| Alternativa | Por que não foi a escolhida |
|---|---|
| Aprovação só pela tela, com troca de chaves por ECDH | sem segredo fora de banda, quem estiver ao alcance na instalação pode se passar pelo sensor; exige criptografia de curva elíptica nos dois lados |
| PAKE com código curto (SPAKE2+ ou EC-JPAKE, como Matter e Thread) | permite um código de 8 dígitos, mas exige curva elíptica e um protocolo bem mais complexo; pode substituir o código de 128 bits em uma versão futura |
| Chave aleatória por sensor, distribuída pela estação aos Nodes | o CAN não é confidencial e a reassociação passaria a depender da estação ligada |

## 8. Validação prevista

Testes na biblioteca (montagem e abertura do pacote, tag adulterada, código errado), cenários na bancada virtual (comissionamento com e sem perda de pedaços, sensor já comissionado, pedido forjado) e roteiro em bancada física com sensor recém-gravado com o firmware genérico.
