// Teste da TransacaoConfig no PC (sem placa).
//
// Compilar e rodar, da raiz do repositório:
//   g++ -std=c++17 -Wall -Iinclude test/host/teste_transacao.cpp -o teste_transacao
//   ./teste_transacao
//
// Fica fora do "pio test" de propósito (a pasta não começa com "test_").

#include "transacao_config.h"

#include <cstdio>
#include <map>

// Flash de mentira: um mapa nome -> conteúdo.
class FlashNaMemoria : public Armazenamento {
public:
    std::map<std::string, std::string> arquivos;
    bool falharGravacao = false;

    std::vector<std::string> listar() override {
        std::vector<std::string> nomes;
        for (auto &a : arquivos) nomes.push_back(a.first);
        return nomes;
    }
    bool gravar(const std::string &nome, const std::string &conteudo) override {
        if (falharGravacao) return false;
        arquivos[nome] = conteudo;
        return true;
    }
    bool ler(const std::string &nome, std::string &conteudo) override {
        auto it = arquivos.find(nome);
        if (it == arquivos.end()) return false;
        conteudo = it->second;
        return true;
    }
    bool apagar(const std::string &nome) override { return arquivos.erase(nome) > 0; }
    bool renomear(const std::string &de, const std::string &para) override {
        auto it = arquivos.find(de);
        if (it == arquivos.end()) return false;
        arquivos[para] = it->second;
        arquivos.erase(it);
        return true;
    }
    bool tem(const std::string &nome) const { return arquivos.count(nome) > 0; }
    std::string conteudo(const std::string &nome) const {
        auto it = arquivos.find(nome);
        return it == arquivos.end() ? std::string("<nao existe>") : it->second;
    }
    bool temRascunho() const {
        for (auto &a : arquivos) if (!a.first.empty() && a.first[0] == '~') return true;
        return false;
    }
};

static int falhas = 0;
static int testes = 0;

#define CONFERE(cond) do { ++testes; if (!(cond)) { ++falhas; \
    std::printf("  FALHOU (linha %d): %s\n", __LINE__, #cond); } } while (0)

// Placa com uma config antiga já instalada.
static FlashNaMemoria placaComConfigAntiga() {
    FlashNaMemoria f;
    f.arquivos["nodeID.txt"] = "CI 101";
    f.arquivos["Ligar.txt"] = "velho-ligar";
    f.arquivos["Desligar.txt"] = "velho-desligar";
    f.arquivos["18.txt"] = "velho-18";
    f.arquivos["_agenda.cfg"] = "1,7,0,Ligar\n";
    f.arquivos["_config.hash"] = "aaaa1111";
    return f;
}

static void testeTransacaoCompleta() {
    std::printf("Transacao completa\n");
    FlashNaMemoria f = placaComConfigAntiga();
    TransacaoConfig t(f);

    CONFERE(t.inicio("bbbb2222", 3, 2) == TransacaoConfig::ACEITO);
    CONFERE(t.codigo("bbbb2222", "Ligar", "novo-ligar") == TransacaoConfig::ACEITO);
    CONFERE(t.codigo("bbbb2222", "Desligar", "novo-desligar") == TransacaoConfig::ACEITO);
    CONFERE(t.codigo("bbbb2222", "22", "novo-22") == TransacaoConfig::ACEITO);
    CONFERE(t.agenda("bbbb2222", 2, "5,18,0,Desligar\n") == TransacaoConfig::ACEITO);   // fora de ordem
    CONFERE(t.agenda("bbbb2222", 1, "1,7,0,22\n") == TransacaoConfig::ACEITO);

    // Antes do Config_Fim, nada da config atual mudou.
    CONFERE(f.conteudo("Ligar.txt") == "velho-ligar");
    CONFERE(f.conteudo("_config.hash") == "aaaa1111");

    CONFERE(t.fim("bbbb2222") == TransacaoConfig::APLICADA);
    CONFERE(f.conteudo("Ligar.txt") == "novo-ligar");
    CONFERE(f.conteudo("22.txt") == "novo-22");
    CONFERE(!f.tem("18.txt"));                                   // não veio: saiu
    CONFERE(f.conteudo("_agenda.cfg") == "1,7,0,22\n5,18,0,Desligar\n");   // partes em ordem
    CONFERE(f.conteudo("_config.hash") == "bbbb2222");
    CONFERE(f.conteudo("nodeID.txt") == "CI 101");               // ID intocado
    CONFERE(!f.temRascunho());
    CONFERE(!t.ativa());
}

static void testeFaltouParte() {
    std::printf("Faltou uma parte: config antiga continua inteira\n");
    FlashNaMemoria f = placaComConfigAntiga();
    TransacaoConfig t(f);

    t.inicio("bbbb2222", 3, 1);
    t.codigo("bbbb2222", "Ligar", "novo-ligar");
    t.codigo("bbbb2222", "Desligar", "novo-desligar");
    // o código "22" se perdeu na malha
    t.agenda("bbbb2222", 1, "1,7,0,22\n");

    CONFERE(t.fim("bbbb2222") == TransacaoConfig::INCOMPLETA);
    CONFERE(t.ultimoMotivo() == "faltam 1 codigo(s) e 0 parte(s) da agenda");
    CONFERE(f.conteudo("Ligar.txt") == "velho-ligar");
    CONFERE(f.conteudo("18.txt") == "velho-18");
    CONFERE(f.conteudo("_agenda.cfg") == "1,7,0,Ligar\n");
    CONFERE(f.conteudo("_config.hash") == "aaaa1111");
    CONFERE(!f.temRascunho());
}

static void testeMensagensRepetidas() {
    std::printf("Mensagens repetidas nao estragam nada\n");
    FlashNaMemoria f = placaComConfigAntiga();
    TransacaoConfig t(f);

    t.inicio("bbbb2222", 2, 1);
    t.codigo("bbbb2222", "Ligar", "novo-ligar");
    CONFERE(t.inicio("bbbb2222", 2, 1) == TransacaoConfig::IGNORADO);     // não zera o rascunho
    CONFERE(f.tem("~Ligar.txt"));
    CONFERE(t.codigo("bbbb2222", "Ligar", "novo-ligar") == TransacaoConfig::ACEITO);  // regrava igual
    t.codigo("bbbb2222", "Desligar", "novo-desligar");
    t.agenda("bbbb2222", 1, "1,7,0,Ligar\n");
    CONFERE(t.fim("bbbb2222") == TransacaoConfig::APLICADA);
    CONFERE(t.fim("bbbb2222") == TransacaoConfig::IGNORADO);              // Config_Fim repetido
    CONFERE(f.conteudo("_config.hash") == "bbbb2222");
}

static void testeHashDiferente() {
    std::printf("Parte de outra transacao e ignorada; Config_Inicio novo descarta o rascunho velho\n");
    FlashNaMemoria f = placaComConfigAntiga();
    TransacaoConfig t(f);

    t.inicio("bbbb2222", 1, 0);
    t.codigo("bbbb2222", "Ligar", "rascunho-velho");
    CONFERE(t.codigo("cccc3333", "Ligar", "intruso") == TransacaoConfig::IGNORADO);
    CONFERE(f.conteudo("~Ligar.txt") == "rascunho-velho");

    CONFERE(t.inicio("cccc3333", 1, 0) == TransacaoConfig::ACEITO);
    CONFERE(!f.temRascunho());
    CONFERE(t.fim("bbbb2222") == TransacaoConfig::IGNORADO);              // fim da transação velha
    t.codigo("cccc3333", "Ligar", "novo");
    CONFERE(t.fim("cccc3333") == TransacaoConfig::APLICADA);
    CONFERE(f.conteudo("Ligar.txt") == "novo");
    CONFERE(f.conteudo("_agenda.cfg") == "");                             // sala sem agenda
}

static void testeForaDeTransacao() {
    std::printf("Partes sem Config_Inicio sao ignoradas\n");
    FlashNaMemoria f = placaComConfigAntiga();
    TransacaoConfig t(f);
    CONFERE(t.codigo("bbbb2222", "Ligar", "x") == TransacaoConfig::IGNORADO);
    CONFERE(t.agenda("bbbb2222", 1, "x") == TransacaoConfig::IGNORADO);
    CONFERE(t.fim("bbbb2222") == TransacaoConfig::IGNORADO);
    CONFERE(f.conteudo("Ligar.txt") == "velho-ligar");
}

static void testeNomesInvalidos() {
    std::printf("Nomes perigosos sao recusados\n");
    FlashNaMemoria f = placaComConfigAntiga();
    TransacaoConfig t(f);
    t.inicio("bbbb2222", 1, 1);
    CONFERE(t.codigo("bbbb2222", "../nodeID", "x") == TransacaoConfig::INVALIDO);
    CONFERE(t.codigo("bbbb2222", "nodeID", "x") == TransacaoConfig::INVALIDO);
    CONFERE(t.codigo("bbbb2222", "_config", "x") == TransacaoConfig::INVALIDO);
    CONFERE(t.codigo("bbbb2222", "a/b", "x") == TransacaoConfig::INVALIDO);
    CONFERE(t.codigo("bbbb2222", "", "x") == TransacaoConfig::INVALIDO);
    CONFERE(t.codigo("bbbb2222", "Ligar", "") == TransacaoConfig::INVALIDO);
    CONFERE(t.agenda("bbbb2222", 0, "x") == TransacaoConfig::INVALIDO);
    CONFERE(t.agenda("bbbb2222", 2, "x") == TransacaoConfig::INVALIDO);   // só 1 parte anunciada
    CONFERE(t.fim("bbbb2222") == TransacaoConfig::INCOMPLETA);
    CONFERE(f.conteudo("nodeID.txt") == "CI 101");
}

static void testeBootLimpaRascunhos() {
    std::printf("Boot apaga rascunhos esquecidos\n");
    FlashNaMemoria f = placaComConfigAntiga();
    f.arquivos["~Ligar.txt"] = "sobrou";
    f.arquivos["~agenda_1.cfg"] = "sobrou";
    TransacaoConfig t(f);
    t.limparRascunhos();
    CONFERE(!f.temRascunho());
    CONFERE(f.conteudo("Ligar.txt") == "velho-ligar");
}

static void testeFalhaNaFlash() {
    std::printf("Falha ao gravar o rascunho e avisada\n");
    FlashNaMemoria f = placaComConfigAntiga();
    TransacaoConfig t(f);
    t.inicio("bbbb2222", 1, 0);
    f.falharGravacao = true;
    CONFERE(t.codigo("bbbb2222", "Ligar", "x") == TransacaoConfig::ERRO_ARQUIVO);
    f.falharGravacao = false;
    CONFERE(t.fim("bbbb2222") == TransacaoConfig::INCOMPLETA);
    CONFERE(f.conteudo("_config.hash") == "aaaa1111");
}

int main() {
    testeTransacaoCompleta();
    testeFaltouParte();
    testeMensagensRepetidas();
    testeHashDiferente();
    testeForaDeTransacao();
    testeNomesInvalidos();
    testeBootLimpaRascunhos();
    testeFalhaNaFlash();
    std::printf("\n%d verificacoes, %d falha(s)\n", testes, falhas);
    return falhas == 0 ? 0 : 1;
}
