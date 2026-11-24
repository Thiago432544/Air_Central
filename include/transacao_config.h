#ifndef TRANSACAO_CONFIG_H
#define TRANSACAO_CONFIG_H

// =======================================================================
// Config completa e atômica da sala (pendência 5).
//
// A central manda, para uma sala:
//   Config_Inicio {hash, codigos: N, agendas: M}
//   Config_Codigo {hash, nome, dados}      x N
//   Config_Agenda {hash, parte, itens}     x M  (parte = 1..M)
//   Config_Fim    {hash}
//
// Tudo é gravado como rascunho ("~..."). Só no Config_Fim, e só se chegou
// tudo o que o Config_Inicio anunciou, os rascunhos viram a config atual.
// Se faltou alguma coisa, o rascunho é apagado e a config antiga continua
// inteira: a placa nunca fica meio configurada.
//
// Esta classe não conhece o Arduino nem a LittleFS: os arquivos passam por
// uma interface (Armazenamento), para a mesma lógica rodar na placa e no
// teste do PC (test/host/teste_transacao.cpp).
//
// Confiabilidade de cada mensagem (Ack, reenvio) é da tarefa 2. Aqui só
// garantimos que mensagens repetidas não estragam nada: um Config_Inicio
// com o hash da transação em andamento é ignorado, e uma parte repetida
// só regrava o mesmo rascunho.
// =======================================================================

#include <set>
#include <string>
#include <vector>

class Armazenamento {
public:
    virtual ~Armazenamento() {}
    // Nomes de arquivo sem a barra inicial (ex.: "Ligar.txt").
    virtual std::vector<std::string> listar() = 0;
    virtual bool gravar(const std::string &nome, const std::string &conteudo) = 0;
    virtual bool ler(const std::string &nome, std::string &conteudo) = 0;
    virtual bool apagar(const std::string &nome) = 0;
    // Substitui o destino se ele existir.
    virtual bool renomear(const std::string &de, const std::string &para) = 0;
};

class TransacaoConfig {
public:
    enum Resultado {
        ACEITO,         // mensagem usada
        IGNORADO,       // fora de uma transação, hash diferente ou repetida
        INVALIDO,       // nome de código ou parte inválidos
        APLICADA,       // Config_Fim: config nova em uso
        INCOMPLETA,     // Config_Fim: faltou parte; config antiga mantida
        ERRO_ARQUIVO    // falha ao gravar na flash
    };

    static constexpr const char *ARQUIVO_HASH = "_config.hash";
    static constexpr const char *ARQUIVO_AGENDA = "_agenda.cfg";

    explicit TransacaoConfig(Armazenamento &fs) : fs(fs) {}

    bool ativa() const { return emAndamento; }
    const std::string &hashAtual() const { return hash; }
    std::string ultimoMotivo() const { return motivo; }

    // Apaga rascunhos esquecidos (boot ou transação abandonada).
    void limparRascunhos() {
        for (const std::string &nome : fs.listar()) {
            if (!nome.empty() && nome[0] == '~') fs.apagar(nome);
        }
    }

    Resultado inicio(const std::string &novoHash, int codigos, int agendas) {
        if (novoHash.empty() || codigos < 0 || agendas < 0) return INVALIDO;
        if (emAndamento && novoHash == hash) return IGNORADO;   // repetido

        limparRascunhos();   // transação anterior abandonada, se houver
        emAndamento = true;
        hash = novoHash;
        esperadosCodigos = codigos;
        esperadasAgendas = agendas;
        codigosRecebidos.clear();
        agendasRecebidas.clear();
        motivo.clear();
        return ACEITO;
    }

    Resultado codigo(const std::string &h, const std::string &nome, const std::string &dados) {
        if (!emAndamento || h != hash) return IGNORADO;
        if (!nomeValido(nome) || dados.empty()) return INVALIDO;
        if (!fs.gravar("~" + nome + ".txt", dados)) return ERRO_ARQUIVO;
        codigosRecebidos.insert(nome);
        return ACEITO;
    }

    // linhas: uma por agendamento, "dia,hora,minuto,codigo\n" (formato do _agenda.cfg).
    Resultado agenda(const std::string &h, int parte, const std::string &linhas) {
        if (!emAndamento || h != hash) return IGNORADO;
        if (parte < 1 || parte > esperadasAgendas) return INVALIDO;
        if (!fs.gravar("~agenda_" + std::to_string(parte) + ".cfg", linhas)) return ERRO_ARQUIVO;
        agendasRecebidas.insert(parte);
        return ACEITO;
    }

    Resultado fim(const std::string &h) {
        if (!emAndamento || h != hash) return IGNORADO;
        emAndamento = false;

        int faltamCodigos = esperadosCodigos - (int) codigosRecebidos.size();
        int faltamAgendas = esperadasAgendas - (int) agendasRecebidas.size();
        if (faltamCodigos != 0 || faltamAgendas != 0) {
            motivo = "faltam " + std::to_string(faltamCodigos) + " codigo(s) e "
                     + std::to_string(faltamAgendas) + " parte(s) da agenda";
            limparRascunhos();
            return INCOMPLETA;
        }
        return aplicar() ? APLICADA : ERRO_ARQUIVO;
    }

    // Nomes de código aceitos: os do firmware ("Ligar", "Desligar", "16".."30")
    // ou parecidos. Nada de barra, ponto ou nomes reservados, para um nome
    // não virar caminho nem apagar arquivo de sistema.
    static bool nomeValido(const std::string &nome) {
        if (nome.empty() || nome.size() > 24) return false;
        if (nome[0] == '~' || nome[0] == '_' || nome == "nodeID") return false;
        for (char c : nome) {
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                   || (c >= '0' && c <= '9') || c == '-' || c == ' ';
            if (!ok) return false;
        }
        return true;
    }

private:
    bool aplicar() {
        // 1. Sem hash durante a troca: se faltar energia no meio, a placa
        //    volta sem hash e a central sabe que precisa reenviar.
        fs.apagar(ARQUIVO_HASH);

        // 2. Agenda: junta as partes, na ordem, num rascunho só.
        std::string agendaNova;
        for (int parte = 1; parte <= esperadasAgendas; ++parte) {
            std::string pedaco;
            if (!fs.ler("~agenda_" + std::to_string(parte) + ".cfg", pedaco)) {
                motivo = "falha ao ler a parte " + std::to_string(parte) + " da agenda";
                limparRascunhos();
                return false;
            }
            agendaNova += pedaco;
        }

        // 3. Códigos que não vieram nesta config saem.
        for (const std::string &arquivo : fs.listar()) {
            if (!ehArquivoDeCodigo(arquivo)) continue;
            std::string nome = arquivo.substr(0, arquivo.size() - 4);
            if (codigosRecebidos.count(nome) == 0) fs.apagar(arquivo);
        }

        // 4. Rascunhos viram os arquivos atuais.
        bool ok = true;
        for (const std::string &nome : codigosRecebidos) {
            ok = fs.renomear("~" + nome + ".txt", nome + ".txt") && ok;
        }
        ok = fs.gravar(ARQUIVO_AGENDA, agendaNova) && ok;
        limparRascunhos();

        // 5. Hash por último: só existe se a troca terminou.
        if (ok) ok = fs.gravar(ARQUIVO_HASH, hash);
        if (!ok) motivo = "falha ao gravar na flash";
        return ok;
    }

    static bool ehArquivoDeCodigo(const std::string &arquivo) {
        if (arquivo.size() < 5) return false;
        if (arquivo.compare(arquivo.size() - 4, 4, ".txt") != 0) return false;
        return nomeValido(arquivo.substr(0, arquivo.size() - 4));
    }

    Armazenamento &fs;
    bool emAndamento = false;
    std::string hash;
    int esperadosCodigos = 0;
    int esperadasAgendas = 0;
    std::set<std::string> codigosRecebidos;
    std::set<int> agendasRecebidas;
    std::string motivo;
};

#endif // TRANSACAO_CONFIG_H
