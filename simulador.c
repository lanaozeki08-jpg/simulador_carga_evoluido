#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_SESSOES 10
#define MAX_FILA 20
#define MAX_NOME 50
#define MAX_DEMANDA_KW 200.0f
#define LIMITE_THROTTLE 0.80f
#define LIMITE_FILA 0.95f
#define TAXA_FIXA 2.50f
#define MULTA_OCIOSIDADE 5.00f
#define OCPP_VERSAO "OCPP 1.6J"
#define CAP_BATERIA_KWH 60.0f

typedef enum {
    TIPO_AC_LENTO = 1,
    TIPO_AC_RAPIDO = 2,
    TIPO_DC_ULTRA = 3
} TipoCarregador;

typedef enum {
    SESS_INATIVA = 0,
    SESS_ATIVA = 1,
    SESS_THROTTLE = 2,
    SESS_CONCLUIDA = 3,
    SESS_AGUARDANDO = 4
} StatusSessao;

typedef struct {
    int id, horaInicio, tempoConectado, tempoEstimado, minutosExtras;
    char usuario[MAX_NOME], ocppTxId[20];
    TipoCarregador tipo;
    StatusSessao status;
    float bateriaInicial, bateriaAtual, kwhConsumido, potenciaAtual, tarifaKwh, valorTotal;
} Sessao;

typedef struct {
    char usuario[MAX_NOME];
    TipoCarregador tipo;
    float bateriaInicial;
    int horaInicio;
    int tempoConectado;
} ItemFila;

static Sessao sessoes[MAX_SESSOES];
static ItemFila fila[MAX_FILA];
static int tamFila = 0;
static int totalSessoes = 0;
static int proximoId = 1;

static float receitaTotal = 0.0f;
static float kwhTotal = 0.0f;
static int sessoesFinalizadas = 0;
static int tempoTotalMin = 0;

float demandaTotal(void);
void aplicarControleDemanda(void);
void promoverDaFila(void);

void limparTela(void) {
#ifdef _WIN32
    system("cls");
#else
    printf("\033[2J\033[H");
#endif
}

void pausar(void) {
    printf("\n  Pressione ENTER para continuar...");
    while (getchar() != '\n')
        ;
    getchar();
}

void barra(const char *label, float pct, int width) {
    int filled = (int)(pct / 100.0f * width);
    if (filled > width)
        filled = width;
    printf("  %-16s [", label);
    for (int i = 0; i < width; i++)
        printf(i < filled ? "#" : ".");
    printf("] %5.1f%%\n", pct);
}

void spinner(const char *msg, int ciclos) {
    const char frames[] = "|/-\\";
    printf("  %s ", msg);
    fflush(stdout);
    for (int i = 0; i < ciclos; i++) {
        printf("\b%c", frames[i % 4]);
        fflush(stdout);
        for (volatile long j = 0; j < 6000000L; j++)
            ;
    }
    printf("\b[OK]\n");
}

void cabecalho(const char *titulo) {
    printf("\n");
    printf("  +================================================+\n");
    printf("  |        * CHARGEGRID INTELLIGENCE *             |\n");
    printf("  +================================================+\n");
    printf("  |  %-46s|\n", titulo);
    printf("  +================================================+\n\n");
}

void sep(void) { printf("  ------------------------------------------------\n"); }
void sep2(void) { printf("  ================================================\n"); }

float potenciaBase(TipoCarregador t) {
    switch (t) {
    case TIPO_AC_LENTO:
        return 7.0f;
    case TIPO_AC_RAPIDO:
        return 22.0f;
    case TIPO_DC_ULTRA:
        return 50.0f;
    default:
        return 7.0f;
    }
}

const char *nomeTipo(TipoCarregador t) {
    switch (t) {
    case TIPO_AC_LENTO:
        return "AC Lento       ( 7 kW)";
    case TIPO_AC_RAPIDO:
        return "AC Semirrapido (22 kW)";
    case TIPO_DC_ULTRA:
        return "DC Rapido      (50 kW)";
    default:
        return "Desconhecido          ";
    }
}

const char *nomeStatus(StatusSessao s) {
    switch (s) {
    case SESS_INATIVA:
        return "Inativa   ";
    case SESS_ATIVA:
        return "Ativa     ";
    case SESS_THROTTLE:
        return "Throttle  ";
    case SESS_CONCLUIDA:
        return "Concluida ";
    case SESS_AGUARDANDO:
        return "Aguardando";
    default:
        return "?         ";
    }
}

float calcTarifa(TipoCarregador tipo, int horaMin) {
    float base;
    switch (tipo) {
    case TIPO_AC_LENTO:
        base = 3.50f;
        break;
    case TIPO_AC_RAPIDO:
        base = 4.20f;
        break;
    case TIPO_DC_ULTRA:
        base = 5.20f;
        break;
    default:
        base = 3.50f;
    }
    if (horaMin >= 1020 && horaMin < 1320)
        base *= 1.20f;
    else if (horaMin < 360)
        base *= 0.85f;
    if (demandaTotal() / MAX_DEMANDA_KW > LIMITE_THROTTLE)
        base *= 1.10f;
    return base;
}

int estimarTempo(float batInicial, float potKw) {
    float bat = batInicial;
    int min = 0;
    float taxaR = (potKw / CAP_BATERIA_KWH * 100.0f) / 60.0f;
    float taxaL = taxaR * 0.45f;
    while (bat < 100.0f) {
        bat += (bat < 80.0f) ? taxaR : taxaL;
        if (bat > 100.0f)
            bat = 100.0f;
        min++;
        if (min > 9999)
            break;
    }
    return min;
}

void gerarTxId(char *buf, int id) {
    sprintf(buf, "TXN-%04d-%05d", id, (id * 13337) % 99999);
}

float demandaTotal(void) {
    float total = 0.0f;
    for (int i = 0; i < MAX_SESSOES; i++)
        if (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE)
            total += sessoes[i].potenciaAtual;
    return total;
}

void aplicarControleDemanda(void) {
    float dem = demandaTotal();
    float ratio = dem / MAX_DEMANDA_KW;

    if (ratio <= LIMITE_THROTTLE) {
        int restaurou = 0;
        for (int i = 0; i < MAX_SESSOES; i++) {
            if (sessoes[i].status == SESS_THROTTLE) {
                sessoes[i].potenciaAtual = potenciaBase(sessoes[i].tipo);
                sessoes[i].status = SESS_ATIVA;
                sessoes[i].tempoEstimado = estimarTempo(sessoes[i].bateriaAtual, sessoes[i].potenciaAtual);
                printf("  [^] Sessao #%d (%s): throttle removido -> %.1f kW\n",
                       sessoes[i].id, sessoes[i].usuario, sessoes[i].potenciaAtual);
                restaurou = 1;
            }
        }
        if (restaurou)
            printf("\n");
        return;
    }

    float fator = (ratio >= LIMITE_FILA) ? 0.50f : 0.75f;
    printf("\n  [!] CONTROLE DE DEMANDA ATIVADO\n");
    printf("  Demanda: %.1f kW / %.0f kW (%.0f%%)\n\n",
           dem, MAX_DEMANDA_KW, ratio * 100.0f);

    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE) {
            float nova = potenciaBase(sessoes[i].tipo) * fator;
            sessoes[i].potenciaAtual = nova;
            sessoes[i].status = SESS_THROTTLE;
            sessoes[i].tempoEstimado = estimarTempo(sessoes[i].bateriaAtual, nova);
            printf("  [v] Sessao #%d (%s): %.1f kW -> %.1f kW\n",
                   sessoes[i].id, sessoes[i].usuario,
                   potenciaBase(sessoes[i].tipo), nova);
        }
    }
    printf("\n");
}

int posicaoNaFila(const char *usuario) {
    for (int i = 0; i < tamFila; i++)
        if (strcmp(fila[i].usuario, usuario) == 0)
            return i + 1;
    return 0;
}

void exibirFila(void) {
    if (tamFila == 0) {
        printf("  Fila de espera vazia.\n");
        return;
    }
    printf("  +--  FILA DE ESPERA (%d aguardando)  -----------+\n", tamFila);
    for (int i = 0; i < tamFila; i++) {
        printf("  | Pos %-2d  %-20s  %s |\n",
               i + 1, fila[i].usuario, nomeTipo(fila[i].tipo));
    }
    printf("  +------------------------------------------------+\n");
}

void promoverDaFila(void) {
    if (tamFila == 0)
        return;

    int slotLivre = -1;
    for (int i = 0; i < MAX_SESSOES; i++)
        if (sessoes[i].status == SESS_INATIVA || sessoes[i].status == SESS_CONCLUIDA) {
            slotLivre = i;
            break;
        }
    if (slotLivre < 0)
        return;

    float dem = demandaTotal();
    if (dem / MAX_DEMANDA_KW >= LIMITE_FILA)
        return;

    ItemFila prom = fila[0];
    for (int i = 0; i < tamFila - 1; i++)
        fila[i] = fila[i + 1];
    tamFila--;

    Sessao *s = &sessoes[slotLivre];
    memset(s, 0, sizeof(Sessao));
    s->id = proximoId++;
    strcpy(s->usuario, prom.usuario);
    s->tipo = prom.tipo;
    s->bateriaInicial = prom.bateriaInicial;
    s->bateriaAtual = prom.bateriaInicial;
    s->horaInicio = prom.horaInicio;
    s->tempoConectado = prom.tempoConectado;
    s->potenciaAtual = potenciaBase(prom.tipo);
    s->tarifaKwh = calcTarifa(prom.tipo, prom.horaInicio);
    s->tempoEstimado = estimarTempo(prom.bateriaInicial, s->potenciaAtual);
    gerarTxId(s->ocppTxId, s->id);
    s->status = SESS_ATIVA;
    totalSessoes++;

    printf("\n  [>>] FILA: '%s' promovido(a) -> Sessao #%d iniciada!\n",
           s->usuario, s->id);
    printf("  Posicoes restantes na fila: %d\n", tamFila);
}

void adicionarNaFila(const char *usuario, TipoCarregador tipo, float batInicial, int horaInicio, int tempoConectado) {
    if (tamFila >= MAX_FILA) {
        printf("  [X] Fila lotada! Tente novamente mais tarde.\n");
        return;
    }
    ItemFila *item = &fila[tamFila++];
    strncpy(item->usuario, usuario, MAX_NOME - 1);
    item->tipo = tipo;
    item->bateriaInicial = batInicial;
    item->horaInicio = horaInicio;
    item->tempoConectado = tempoConectado;

    int posicao = posicaoNaFila(usuario);

    printf("\n  [!] Demanda no limite! Sessao adicionada a fila.\n");
    printf("  Posicao: %d de %d\n", posicao, tamFila);
    printf("  Voce sera promovido automaticamente quando houver capacidade.\n");
}

void ocppEnviar(const char *tipo, const char *payload) {
    printf("  +- [%s] TX  %-30s +\n", OCPP_VERSAO, tipo);
    printf("  |  %s\n", payload);
    printf("  +--------------------------------------------------+\n");
}

void ocppReceber(const char *tipo, const char *resp) {
    printf("  +- [%s] RX  %-30s +\n", OCPP_VERSAO, tipo);
    printf("  |  %s\n", resp);
    printf("  +--------------------------------------------------+\n\n");
}

void modbusLog(const char *reg, float val, const char *un) {
    printf("  [MODBUS] %-22s = %8.2f %s\n", reg, val, un);
}

void simularConexaoOCPP(Sessao *s) {
    char buf[120];
    spinner("Conectando ao servidor OCPP", 18);
    sprintf(buf, "{\"connectorId\":%d,\"idTag\":\"%s\",\"meterStart\":0}", s->id, s->usuario);
    ocppEnviar("StartTransaction.req", buf);
    sprintf(buf, "{\"transactionId\":\"%s\",\"status\":\"Accepted\"}", s->ocppTxId);
    ocppReceber("StartTransaction.conf", buf);
    ocppEnviar("Heartbeat.req", "{}");
    ocppReceber("Heartbeat.conf", "{\"currentTime\":\"2025-06-11T10:00:00Z\"}");
    modbusLog("PowerActive_W", s->potenciaAtual * 1000, "W");
    modbusLog("BatterySOC_%", s->bateriaAtual, "%");
    modbusLog("GridDemand_kW", demandaTotal(), "kW");
}

void simularEncerramentoOCPP(Sessao *s) {
    char buf[120];
    spinner("Encerrando no servidor OCPP", 18);
    sprintf(buf, "{\"transactionId\":\"%s\",\"meterStop\":%.0f,\"reason\":\"Local\"}",
            s->ocppTxId, s->kwhConsumido * 1000);
    ocppEnviar("StopTransaction.req", buf);
    ocppReceber("StopTransaction.conf", "{\"status\":\"Accepted\"}");
    spinner("Sincronizando GoodWe Cloud", 14);
    printf("  [GoodWe] Sessao %s registrada no dashboard.\n\n", s->ocppTxId);
}

void abrirSessao(void) {
    cabecalho("NOVA SESSAO DE RECARGA");

    int slotLivre = -1;
    for (int i = 0; i < MAX_SESSOES; i++)
        if (sessoes[i].status == SESS_INATIVA || sessoes[i].status == SESS_CONCLUIDA) {
            slotLivre = i;
            break;
        }
    if (slotLivre < 0) {
        printf("  [X] Nenhum slot disponivel. Todos os %d conectores ocupados.\n",
               MAX_SESSOES);
        pausar();
        return;
    }

    char nome[MAX_NOME];
    TipoCarregador tipo;
    float batInicial;
    int hora, minuto = 0, tempoConec, nomeValido;

    do {
    nome[0] = '\0';
    printf("  Nome do usuario: ");
    int r = scanf(" %49[^\n]", nome);

    nomeValido = 1;

    if (r != 1 || nome[0] == '\0') {
        nomeValido = 0;
    } else {
        for (int i = 0; nome[i] != '\0'; i++) {
            if (isdigit((unsigned char)nome[i])) {
                nomeValido = 0;
                break;
            }
        }
    }

    if (!nomeValido)
        printf("  [!] Nome invalido (nao pode ser vazio ou conter numeros). Tente novamente.\n");

    } while (!nomeValido);

    int t;
    do {
        printf("\n  Tipo de carregador:\n");
        printf("    [1] AC Lento        7 kW   R$ 3.50/kWh base\n");
        printf("    [2] AC Semirrapido  22 kW   R$ 4.20/kWh base\n");
        printf("    [3] DC Rapido       50 kW   R$ 5.20/kWh base\n");
        printf("  Escolha: ");
        if (scanf("%d", &t) != 1 || t < 1 || t > 3) {
            printf("  [!] Opcao invalida.\n");
            while (getchar() != '\n')
                ;
            t = 0;
        }
    } while (t < 1 || t > 3);
    tipo = (TipoCarregador)t;

    do {
        printf("  Bateria inicial (%% 0-99): ");
        if (scanf("%f", &batInicial) != 1 || batInicial < 0 || batInicial > 99) {
            printf("  [!] Valor invalido.\n");
            while (getchar() != '\n')
                ;
            batInicial = -1;
        }
    } while (batInicial < 0 || batInicial > 99);

    int tempoEstimadoPreview = estimarTempo(batInicial, potenciaBase(tipo));
    int hEst = tempoEstimadoPreview / 60;
    int mEst = tempoEstimadoPreview % 60;
    printf("\n  +------------------------------------------------+\n");
    printf("  |  PREVISAO DE CARGA COMPLETA (100%%)             |\n");
    printf("  |  Carregador  : %-30s  |\n", nomeTipo(tipo));
    printf("  |  Bateria     : %.1f%% -> 100%%                    |\n", batInicial);
    if (hEst > 0)
        printf("  |  Tempo est.  : %dh %02dmin (%d min total)          |\n",
               hEst, mEst, tempoEstimadoPreview);
    else
        printf("  |  Tempo est.  : %d min                           |\n",
               tempoEstimadoPreview);
    printf("  |  (Tempo acima disso gera multa de R$ %.2f/min) |\n", MULTA_OCIOSIDADE);
    printf("  +------------------------------------------------+\n\n");

    do {
        printf("  Hora de inicio (HH ou HH:MM): ");
        minuto = 0;
        int r = scanf("%d:%d", &hora, &minuto);
        if (r < 1 || hora < 0 || hora > 23 || minuto < 0 || minuto > 59) {
            printf("  [!] Horario invalido.\n");
            while (getchar() != '\n')
                ;
            hora = -1;
        }
    } while (hora < 0 || hora > 23);

    do {
        printf("  Tempo de conexao (minutos) [recomendado: %d]: ", tempoEstimadoPreview);
        if (scanf("%d", &tempoConec) != 1 || tempoConec <= 0) {
            printf("  [!] Tempo invalido.\n");
            while (getchar() != '\n')
                ;
            tempoConec = 0;
        }
    } while (tempoConec <= 0);

    if (tempoConec > tempoEstimadoPreview) {
        int minutosExtras = tempoConec - tempoEstimadoPreview;
        float multaEstimada = minutosExtras * MULTA_OCIOSIDADE;
        printf("\n  [!] AVISO: Tempo informado excede o estimado em %d min.\n", minutosExtras);
        printf("  Multa de ociosidade estimada: R$ %.2f\n", multaEstimada);
        printf("  Deseja continuar assim mesmo? (1=Sim / 0=Nao): ");
        int confirma;
        if (scanf("%d", &confirma) != 1 || confirma == 0) {
            printf("  Operacao cancelada. Informe um novo tempo de conexao.\n");
            pausar();
            return;
        }
    }

    int horaMin = hora * 60 + minuto;

    float dem = demandaTotal();
    if (dem / MAX_DEMANDA_KW >= LIMITE_FILA) {
        adicionarNaFila(nome, tipo, batInicial, horaMin, tempoConec);
        exibirFila();
        pausar();
        return;
    }

    Sessao *s = &sessoes[slotLivre];
    memset(s, 0, sizeof(Sessao));
    s->id = proximoId++;
    strcpy(s->usuario, nome);
    s->tipo = tipo;
    s->bateriaInicial = batInicial;
    s->bateriaAtual = batInicial;
    s->horaInicio = horaMin;
    s->tempoConectado = tempoConec;
    s->potenciaAtual = potenciaBase(tipo);
    s->tarifaKwh = calcTarifa(tipo, horaMin);
    s->tempoEstimado = estimarTempo(batInicial, s->potenciaAtual);
    gerarTxId(s->ocppTxId, s->id);
    s->status = SESS_ATIVA;
    totalSessoes++;

    sep();
    simularConexaoOCPP(s);
    aplicarControleDemanda();

    printf("  [v] Sessao #%d aberta!\n", s->id);
    printf("  Transacao : %s\n", s->ocppTxId);
    printf("  Potencia  : %.1f kW\n", s->potenciaAtual);
    printf("  Tarifa    : R$ %.2f/kWh\n", s->tarifaKwh);
    printf("  Est. tempo: %d min\n", s->tempoEstimado);

    promoverDaFila();

    pausar();
}

void simularSessao(int idx) {
    Sessao *s = &sessoes[idx];
    if (s->status != SESS_ATIVA && s->status != SESS_THROTTLE) {
        printf("  [!] Sessao #%d nao esta ativa.\n", s->id);
        pausar();
        return;
    }

    cabecalho("SIMULANDO SESSAO");
    printf("  Usuario : %s\n", s->usuario);
    printf("  Tipo    : %s\n", nomeTipo(s->tipo));
    printf("  Potencia: %.1f kW\n\n", s->potenciaAtual);

    float taxaG = (s->potenciaAtual / CAP_BATERIA_KWH * 100.0f) / 60.0f;

    for (int t = 1; t <= s->tempoConectado; t++) {
        if (s->bateriaAtual < 100.0f) {
            float taxa = (s->bateriaAtual < 80.0f) ? taxaG : taxaG * 0.45f;
            s->bateriaAtual += taxa;
            if (s->bateriaAtual > 100.0f)
                s->bateriaAtual = 100.0f;
            float kwh = s->potenciaAtual * ((s->bateriaAtual < 80.0f) ? 1.0f : 0.45f) / 60.0f;
            s->kwhConsumido += kwh;
        }
        if (t % 10 == 0 || t == 1 || t == s->tempoConectado) {
            barra("Bateria", s->bateriaAtual, 25);
            printf("  Min %-4d  %.1f%%  %.3f kWh\n\n", t, s->bateriaAtual, s->kwhConsumido);
        }
    }

    if (s->tempoConectado > s->tempoEstimado)
        s->minutosExtras = s->tempoConectado - s->tempoEstimado;

    float multa = s->minutosExtras * MULTA_OCIOSIDADE;
    s->valorTotal = s->kwhConsumido * s->tarifaKwh + TAXA_FIXA + multa;
    s->status = SESS_CONCLUIDA;

    receitaTotal += s->valorTotal;
    kwhTotal += s->kwhConsumido;
    sessoesFinalizadas++;
    tempoTotalMin += s->tempoConectado;

    simularEncerramentoOCPP(s);

    printf("  [v] Concluido!\n");
    printf("  Bateria final : %.1f%%\n", s->bateriaAtual);
    printf("  Energia       : %.3f kWh\n", s->kwhConsumido);
    printf("  Valor total   : R$ %.2f\n", s->valorTotal);

    promoverDaFila();
    if (tamFila > 0)
        exibirFila();

    pausar();
}

void painelSessoes(void) {
    cabecalho("PAINEL DE SESSOES");

    printf("  %-4s %-20s %-19s %-11s %-7s %-7s\n",
           "ID", "Usuario", "Tipo", "Status", "Bat%", "kWh");
    sep();

    int alguma = 0;
    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].status == SESS_INATIVA)
            continue;
        alguma = 1;
        printf("  #%-3d %-20s %-19s %-11s %5.1f%%  %.3f\n",
               sessoes[i].id,
               sessoes[i].usuario,
               nomeTipo(sessoes[i].tipo),
               nomeStatus(sessoes[i].status),
               sessoes[i].bateriaAtual,
               sessoes[i].kwhConsumido);
    }
    if (!alguma)
        printf("  Nenhuma sessao registrada.\n");

    sep();
    float dem = demandaTotal();
    barra("Grid", dem / MAX_DEMANDA_KW * 100.0f, 25);
    printf("  Demanda: %.1f kW / %.0f kW\n\n", dem, MAX_DEMANDA_KW);

    exibirFila();
    pausar();
}

void dashboard(void) {
    limparTela();

    int ativas = 0, fila_cnt = tamFila;
    float demAtual = 0.0f, kwhSessaoAtiva = 0.0f;

    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE) {
            ativas++;
            demAtual += sessoes[i].potenciaAtual;
            kwhSessaoAtiva += sessoes[i].kwhConsumido;
        }
    }

    float utilizacao = demAtual / MAX_DEMANDA_KW * 100.0f;
    float kwhHistorico = kwhTotal + kwhSessaoAtiva;
    float recHistorico = receitaTotal;

    float ticketMedio = (sessoesFinalizadas > 0) ? receitaTotal / sessoesFinalizadas : 0.0f;
    float energiaMedia = (sessoesFinalizadas > 0) ? kwhTotal / sessoesFinalizadas : 0.0f;
    float tempoMedio = (sessoesFinalizadas > 0) ? (float)tempoTotalMin / sessoesFinalizadas : 0.0f;
    float recPorSessao = ticketMedio;

    const char *ocppStatus = (ativas > 0 || tamFila > 0) ? "ONLINE" : "IDLE  ";

    printf("\n");
    sep2();
    printf("  |          DASHBOARD EXECUTIVO                 |\n");
    printf("  |          ChargeGrid Intelligence             |\n");
    sep2();
    printf("\n");

    printf("  [OPERACIONAL]\n");
    sep();
    printf("  Sessoes Ativas    : %d\n", ativas);
    printf("  Na Fila           : %d\n", fila_cnt);
    printf("  Total Historico   : %d sessoes\n", totalSessoes);
    printf("\n");
    barra("Utilizacao Grid", utilizacao, 25);
    barra("Sessoes Ativas", (float)ativas / MAX_SESSOES * 100.0f, 25);
    printf("\n");
    printf("  Demanda Atual     : %.1f kW / %.0f kW\n", demAtual, MAX_DEMANDA_KW);
    printf("  Energia (sessoes) : %.3f kWh\n", kwhSessaoAtiva);
    printf("  OCPP              : [%s]\n", ocppStatus);

    sep();

    printf("  [FINANCEIRO  (sessoes concluidas)]\n");
    sep();
    printf("  Energia Vendida   : %.3f kWh\n", kwhHistorico);
    printf("  Receita Total     : R$ %8.2f\n", recHistorico);

    if (sessoesFinalizadas > 0) {
        printf("  Sessoes Concluidas: %d\n", sessoesFinalizadas);
    }
    else {
        printf("  Sessoes Concluidas: 0  (execute sessoes para ver KPIs)\n");
    }

    sep();

    printf("  [KPIs DE NEGOCIO]\n");
    sep();
    if (sessoesFinalizadas > 0) {
        printf("  Ticket Medio      : R$ %7.2f / sessao\n", ticketMedio);
        printf("  Receita p/ Sessao : R$ %7.2f\n", recPorSessao);
        printf("  Energia Media     : %.3f kWh / sessao\n", energiaMedia);
        printf("  Tempo Medio       : %.1f min / sessao\n", tempoMedio);
        printf("\n");

        float recDia = ticketMedio * 24.0f;
        printf("  Proj. Receita/Dia : R$ %7.2f\n", recDia);
    }
    else {
        printf("  (Nenhuma sessao finalizada ainda)\n");
    }

    sep();

    printf("  [CONECTORES]\n");
    sep();
    int cnt[4] = {0, 0, 0, 0};
    float pot[4] = {0, 0, 0, 0};
    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE) {
            int ti = (int)sessoes[i].tipo;
            cnt[ti]++;
            pot[ti] += sessoes[i].potenciaAtual;
        }
    }
    printf("  AC Lento       ( 7 kW) : %d ativo(s)  %.1f kW\n", cnt[1], pot[1]);
    printf("  AC Semirrapido (22 kW) : %d ativo(s)  %.1f kW\n", cnt[2], pot[2]);
    printf("  DC Rapido      (50 kW) : %d ativo(s)  %.1f kW\n", cnt[3], pot[3]);

    sep2();
    pausar();
}

void relatorioCompleto(void) {
    cabecalho("RELATORIO GERAL DO SISTEMA");

    float totKwh = 0, totRec = 0;
    int conc = 0, atv = 0;

    for (int i = 0; i < MAX_SESSOES; i++) {
        Sessao *s = &sessoes[i];
        if (s->status == SESS_INATIVA)
            continue;

        printf("  +--- Sessao #%d ---------------------------------+\n", s->id);
        printf("  | Usuario       : %-28s |\n", s->usuario);
        printf("  | Tipo          : %-28s |\n", nomeTipo(s->tipo));
        printf("  | Status        : %-28s |\n", nomeStatus(s->status));
        printf("  | Bateria       : %.1f%% -> %.1f%%\n", s->bateriaInicial, s->bateriaAtual);
        printf("  | Energia       : %.3f kWh\n", s->kwhConsumido);
        printf("  | Potencia real : %.1f kW\n", s->potenciaAtual);
        printf("  | Tarifa kWh    : R$ %.2f\n", s->tarifaKwh);
        printf("  | Transacao     : %s\n", s->ocppTxId);

        if (s->status == SESS_CONCLUIDA) {
            float multa = s->minutosExtras * MULTA_OCIOSIDADE;
            printf("  | --- Cobranca ---\n");
            printf("  | Energia       : R$ %.2f\n", s->kwhConsumido * s->tarifaKwh);
            printf("  | Taxa fixa     : R$ %.2f\n", TAXA_FIXA);
            if (multa > 0.0f)
                printf("  | Multa ociosid.: R$ %.2f (%d min)\n", multa, s->minutosExtras);
            printf("  | TOTAL         : R$ %.2f\n", s->valorTotal);
            totKwh += s->kwhConsumido;
            totRec += s->valorTotal;
            conc++;
        }
        else {
            atv++;
        }
        printf("  +------------------------------------------------+\n\n");
    }

    sep();
    printf("  CONSOLIDADO\n");
    printf("  Sessoes ativas    : %d\n", atv);
    printf("  Sessoes concluidas: %d\n", conc);
    printf("  Energia total     : %.3f kWh\n", totKwh);
    printf("  Receita total     : R$ %.2f\n", totRec);
    printf("  Demanda atual     : %.1f kW\n", demandaTotal());
    sep();
    exibirFila();
    pausar();
}

void encerrarSessao(void) {
    cabecalho("ENCERRAR SESSAO");

    int alguma = 0;
    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE) {
            printf("  [%d] %s  |  %s  |  %.1f%%\n",
                   sessoes[i].id, sessoes[i].usuario,
                   nomeTipo(sessoes[i].tipo), sessoes[i].bateriaAtual);
            alguma = 1;
        }
    }
    if (!alguma) {
        printf("  Nenhuma sessao ativa.\n");
        pausar();
        return;
    }

    int escolha;
    printf("\n  ID da sessao a encerrar (0=cancelar): ");
    if (scanf("%d", &escolha) != 1 || escolha == 0) {
        pausar();
        return;
    }

    Sessao *s = NULL;
    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].id == escolha &&
            (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE)) {
            s = &sessoes[i];
            break;
        }
    }
    if (!s) {
        printf("  [!] Sessao nao encontrada.\n");
        pausar();
        return;
    }

    if (s->minutosExtras == 0 && s->tempoConectado > s->tempoEstimado)
        s->minutosExtras = s->tempoConectado - s->tempoEstimado;

    float multa = s->minutosExtras * MULTA_OCIOSIDADE;
    s->valorTotal = s->kwhConsumido * s->tarifaKwh + TAXA_FIXA + multa;
    s->status = SESS_CONCLUIDA;

    receitaTotal += s->valorTotal;
    kwhTotal += s->kwhConsumido;
    sessoesFinalizadas++;
    tempoTotalMin += s->tempoConectado;

    simularEncerramentoOCPP(s);
    printf("  [v] Sessao #%d encerrada. Valor: R$ %.2f\n", s->id, s->valorTotal);

    promoverDaFila();
    if (tamFila > 0)
        exibirFila();

    pausar();
}

void cenarioDemo(void) {
    cabecalho("CENARIO DE DEMONSTRACAO AUTOMATICA");
    printf("  Simulando 4 veiculos (1 vai para a fila)...\n\n");
    spinner("Preparando cenario", 25);

    memset(sessoes, 0, sizeof(sessoes));
    for (int i = 0; i < MAX_SESSOES; i++)
        sessoes[i].status = SESS_INATIVA;
    tamFila = 0;

    struct {
        const char *nome;
        TipoCarregador tipo;
        float bat;
        int hini;
        int tcon;
    } veics[] = {
        {"Ana Costa", TIPO_AC_LENTO, 20.0f, 480, 60},
        {"Bruno Lima", TIPO_AC_RAPIDO, 40.0f, 1080, 45},
        {"Carla Souza", TIPO_DC_ULTRA, 5.0f, 180, 30},
        {"Diego Rocha", TIPO_AC_RAPIDO, 60.0f, 900, 40},
    };

    for (int v = 0; v < 4; v++) {
        float dem = demandaTotal();
        int slotLivre = -1;
        for (int i = 0; i < MAX_SESSOES; i++)
            if (sessoes[i].status == SESS_INATIVA || sessoes[i].status == SESS_CONCLUIDA) {
                slotLivre = i;
                break;
            }

        if (slotLivre >= 0 && dem / MAX_DEMANDA_KW < LIMITE_FILA) {
            Sessao *s = &sessoes[slotLivre];
            memset(s, 0, sizeof(Sessao));
            s->id = proximoId++;
            strcpy(s->usuario, veics[v].nome);
            s->tipo = veics[v].tipo;
            s->bateriaInicial = veics[v].bat;
            s->bateriaAtual = veics[v].bat;
            s->horaInicio = veics[v].hini;
            s->tempoConectado = veics[v].tcon;
            s->potenciaAtual = potenciaBase(veics[v].tipo);
            s->tarifaKwh = calcTarifa(veics[v].tipo, veics[v].hini);
            s->tempoEstimado = estimarTempo(veics[v].bat, s->potenciaAtual);
            gerarTxId(s->ocppTxId, s->id);
            s->status = SESS_ATIVA;
            totalSessoes++;
            printf("  [+] Sessao #%d: %s  (%s)\n",
                   s->id, s->usuario, nomeTipo(s->tipo));
        }
        else {
            adicionarNaFila(veics[v].nome, veics[v].tipo,
                            veics[v].bat, veics[v].hini, veics[v].tcon);
        }
    }

    printf("\n");
    sep();
    float dem = demandaTotal();
    printf("  Demanda apos abertura: %.1f kW (%.0f%%)\n\n",
           dem, dem / MAX_DEMANDA_KW * 100.0f);
    barra("Utilizacao", dem / MAX_DEMANDA_KW * 100.0f, 25);
    printf("\n");
    exibirFila();
    sep();

    aplicarControleDemanda();

    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].status != SESS_ATIVA && sessoes[i].status != SESS_THROTTLE)
            continue;
        Sessao *s = &sessoes[i];

        printf("\n  * Simulando sessao #%d  %s\n", s->id, s->usuario);
        spinner("Processando", 16);

        float taxaG = (s->potenciaAtual / CAP_BATERIA_KWH * 100.0f) / 60.0f;
        for (int t = 1; t <= s->tempoConectado; t++) {
            if (s->bateriaAtual < 100.0f) {
                float taxa = (s->bateriaAtual < 80.0f) ? taxaG : taxaG * 0.45f;
                s->bateriaAtual += taxa;
                if (s->bateriaAtual > 100.0f)
                    s->bateriaAtual = 100.0f;
                s->kwhConsumido += s->potenciaAtual * ((s->bateriaAtual < 80.0f) ? 1.0f : 0.45f) / 60.0f;
            }
        }
        if (s->tempoConectado > s->tempoEstimado)
            s->minutosExtras = s->tempoConectado - s->tempoEstimado;

        float multa = s->minutosExtras * MULTA_OCIOSIDADE;
        s->valorTotal = s->kwhConsumido * s->tarifaKwh + TAXA_FIXA + multa;
        s->status = SESS_CONCLUIDA;

        receitaTotal += s->valorTotal;
        kwhTotal += s->kwhConsumido;
        sessoesFinalizadas++;
        tempoTotalMin += s->tempoConectado;

        barra("Bateria final", s->bateriaAtual, 25);
        printf("  Energia: %.3f kWh  |  R$ %.2f\n",
               s->kwhConsumido, s->valorTotal);

        promoverDaFila();
    }

    printf("\n");
    sep();
    printf("  RESUMO DO CENARIO\n");
    sep();
    float recT = 0, kwhT = 0;
    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].status == SESS_CONCLUIDA) {
            recT += sessoes[i].valorTotal;
            kwhT += sessoes[i].kwhConsumido;
            printf("  %-15s  %.1f%%  %.3f kWh  R$ %.2f\n",
                   sessoes[i].usuario, sessoes[i].bateriaAtual,
                   sessoes[i].kwhConsumido, sessoes[i].valorTotal);
        }
    }
    sep();
    printf("  Energia total  : %.3f kWh\n", kwhT);
    printf("  Receita total  : R$ %.2f\n", recT);
    if (tamFila > 0) {
        printf("\n");
        exibirFila();
    }

    pausar();
}

void menuPrincipal(void) {
    int opc;
    do {
        limparTela();
        printf("\n");
        printf("  +================================================+\n");
        printf("  |                 ASTERCHARGE                    |\n");
        printf("  |     Sistema Inteligente de Recarga EV          |\n");
        printf("  |     GoodWe Ecosystem  |  FIAP                  |\n");
        printf("  +================================================+\n");

        float dem = demandaTotal();
        float ratio = dem / MAX_DEMANDA_KW * 100.0f;
        int filled = (int)(ratio / 100.0f * 16);
        printf("  |  Grid [");
        for (int i = 0; i < 16; i++)
            printf(i < filled ? "#" : ".");
        printf("] %5.1f%%  |\n", ratio);

        int atv = 0;
        for (int i = 0; i < MAX_SESSOES; i++)
            if (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE)
                atv++;
        printf("  |  Ativas: %-2d  Fila: %-2d  Receita: R$ %7.2f  |\n",
               atv, tamFila, receitaTotal);
        printf("  +================================================+\n");
        printf("  |  [1] Abrir nova sessao                         |\n");
        printf("  |  [2] Painel de sessoes + fila                  |\n");
        printf("  |  [3] Simular sessao existente                  |\n");
        printf("  |  [4] >> DASHBOARD EXECUTIVO <<                 |\n");
        printf("  |  [5] Relatorio completo                        |\n");
        printf("  |  [6] Cenario de demonstracao (4 veiculos)      |\n");
        printf("  |  [7] Controle de demanda                       |\n");
        printf("  |  [8] Encerrar sessao                           |\n");
        printf("  |  [0] Sair                                      |\n");
        printf("  +================================================+\n");
        printf("  Opcao: ");

        if (scanf("%d", &opc) != 1) {
            while (getchar() != '\n')
                ;
            opc = -1;
        }

        switch (opc) {
        case 1:
            limparTela();
            abrirSessao();
            break;
        case 2:
            limparTela();
            painelSessoes();
            break;
        case 3:
        {
            limparTela();
            printf("  ID da sessao para simular: ");

            int id;
            if (scanf("%d", &id) != 1) {
                printf("  [!] ID invalido.\n");
                pausar();
                break;
            }

            int indice = -1;

            for (int i = 0; i < MAX_SESSOES; i++) {
                if (sessoes[i].id == id) {
                    indice = i;
                    break;
                }
            }

            if (indice >= 0) {
                simularSessao(indice);
            }
            else {
                printf("  [!] Sessao nao encontrada.\n");
                pausar();
            }

            break;
        }
        case 4:
            dashboard();
            break;
        case 5:
            limparTela();
            relatorioCompleto();
            break;
        case 6:
            limparTela();
            cenarioDemo();
            break;
        case 7:
            limparTela();
            cabecalho("CONTROLE DE DEMANDA");
            aplicarControleDemanda();
            pausar();
            break;
        case 8:
            limparTela();
            encerrarSessao();
            break;
        case 0:
            printf("\n  Ate logo! *\n\n");
            break;
        default:
            printf("  [!] Opcao invalida.\n");
            pausar();
        }
    } while (opc != 0);
}

int main(void) {
    memset(sessoes, 0, sizeof(sessoes));
    for (int i = 0; i < MAX_SESSOES; i++)
        sessoes[i].status = SESS_INATIVA;
    proximoId = 1;
    menuPrincipal();
    return 0;
}