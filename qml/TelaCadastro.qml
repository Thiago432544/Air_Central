import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// =======================================================================
// Tela "Modelos e códigos IR".
// Cada modelo de ar tem até 17 códigos (Ligar, Desligar, 16..30 °C).
// Os dados vêm do Repositorio (contexto "repositorio"); a captura passa
// pela ponte: sendData.require_ir_read() e receiveData.set_waited_capture().
// =======================================================================
Item {
    id: root

    property var modelos: []                 // [{id, nome, marca, codigos, salas}]
    property int modeloId: -1                // modelo selecionado
    property var comandos: ({})              // {"LIGAR": true, ...} do modelo selecionado
    property string comando: "LIGAR"         // comando selecionado
    property bool aguardando: false          // esperando o código IR chegar
    property string mensagem: ""             // aviso curto embaixo dos botões

    readonly property var listaComandos: ["LIGAR", "DESLIGAR",
        "T16", "T17", "T18", "T19", "T20", "T21", "T22", "T23",
        "T24", "T25", "T26", "T27", "T28", "T29", "T30"]

    function rotulo(cmd) {
        if (cmd === "LIGAR") return "Ligar"
        if (cmd === "DESLIGAR") return "Desligar"
        return cmd.substring(1) + "°C"
    }

    function modeloAtual() {
        for (var i = 0; i < modelos.length; i++)
            if (modelos[i].id === modeloId) return modelos[i]
        return null
    }

    function recarregar() {
        modelos = repositorio.listarModelos()
        if (modeloAtual() === null)
            modeloId = modelos.length > 0 ? modelos[0].id : -1
        comandos = modeloId >= 0 ? repositorio.comandosDoModelo(modeloId) : ({})
    }

    function cancelarCaptura() {
        if (aguardando) receiveData.cancel_waited_capture()
        aguardando = false
    }

    function selecionarModelo(id) {
        cancelarCaptura()
        mensagem = ""
        modeloId = id
        comandos = repositorio.comandosDoModelo(id)
    }

    Component.onCompleted: recarregar()
    Component.onDestruction: if (aguardando) receiveData.cancel_waited_capture()

    Connections {
        target: repositorio
        function onModelosAlterados() { root.recarregar() }
    }

    Connections {
        target: receiveData
        function onCodigoCapturado(id, cmd) {
            root.aguardando = false
            root.mensagem = "✓ Código de " + root.rotulo(cmd) + " gravado."
        }
        function onCapturaFalhou(motivo) {
            root.aguardando = false
            root.mensagem = "⚠️ " + motivo
        }
    }

    // Porta serial fechada: a ponte não recebeu o pedido, então não há o que esperar.
    Connections {
        target: sendData
        function onErrorOccurred(errorMessage) { root.cancelarCaptura() }
    }

    // ---------------------------------------------------------------
    // Popup: criar ou renomear modelo
    // ---------------------------------------------------------------
    Popup {
        id: popupModelo
        property bool editando: false
        width: 360
        height: 340
        anchors.centerIn: parent
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        function abrir(editar) {
            editando = editar
            var m = root.modeloAtual()
            campoNomeModelo.text = editar && m ? m.nome : ""
            campoMarca.text = editar && m ? m.marca : ""
            erroModelo.text = ""
            open()
            campoNomeModelo.forceActiveFocus()
        }

        function salvar() {
            var nome = campoNomeModelo.text.trim()
            if (nome === "") { erroModelo.text = "Dê um nome ao modelo."; return }
            if (editando) {
                if (!repositorio.renomearModelo(root.modeloId, nome, campoMarca.text.trim())) {
                    erroModelo.text = "Já existe um modelo com esse nome."
                    return
                }
            } else {
                var id = repositorio.criarModelo(nome, campoMarca.text.trim())
                if (id < 0) { erroModelo.text = "Já existe um modelo com esse nome."; return }
                root.selecionarModelo(id)
            }
            close()
        }

        background: Rectangle { color: "white"; radius: 12; border.color: "#90caf9"; border.width: 2 }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 8

            Text {
                text: popupModelo.editando ? "✏️ Editar modelo" : "➕ Novo modelo"
                font.bold: true; font.pixelSize: 17; color: "#1565c0"
            }
            Text { text: "Nome (ex.: Springer 12k - Lab 1)"; font.pixelSize: 12; color: "#666666" }
            TextField {
                id: campoNomeModelo
                Layout.fillWidth: true
                selectByMouse: true
                onAccepted: popupModelo.salvar()
            }
            Text { text: "Marca (opcional)"; font.pixelSize: 12; color: "#666666" }
            TextField {
                id: campoMarca
                Layout.fillWidth: true
                selectByMouse: true
                onAccepted: popupModelo.salvar()
            }
            Text { id: erroModelo; color: "#d32f2f"; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 10
                Button { text: "Cancelar"; Layout.preferredWidth: 110; onClicked: popupModelo.close() }
                Button {
                    text: "Salvar"
                    Layout.preferredWidth: 110
                    background: Rectangle { color: parent.down ? "#0d47a1" : "#1565c0"; radius: 6 }
                    contentItem: Text { text: parent.text; color: "white"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: popupModelo.salvar()
                }
            }
        }
    }

    // ---------------------------------------------------------------
    // Popup: confirmar exclusão (modelo inteiro ou um código)
    // ---------------------------------------------------------------
    Popup {
        id: popupConfirmar
        property bool apagarModelo: true
        width: 360
        height: 240
        anchors.centerIn: parent
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle { color: "white"; radius: 12; border.color: "#d32f2f"; border.width: 2 }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 12

            Text {
                text: popupConfirmar.apagarModelo ? "🗑️ Apagar modelo" : "🗑️ Apagar código"
                font.bold: true; font.pixelSize: 17; color: "#d32f2f"
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 13
                text: {
                    var m = root.modeloAtual()
                    if (!m) return ""
                    if (!popupConfirmar.apagarModelo)
                        return "Apagar o código de " + root.rotulo(root.comando) + " do modelo \"" + m.nome + "\"?"
                    var aviso = "Apagar o modelo \"" + m.nome + "\" e os " + m.codigos + " códigos dele?"
                    if (m.salas > 0)
                        aviso += " " + m.salas + (m.salas === 1 ? " sala usa" : " salas usam") + " este modelo e vai" + (m.salas === 1 ? "" : "o") + " ficar sem modelo."
                    return aviso
                }
            }
            Item { Layout.fillHeight: true }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 10
                Button { text: "Cancelar"; Layout.preferredWidth: 110; onClicked: popupConfirmar.close() }
                Button {
                    text: "Sim, apagar"
                    Layout.preferredWidth: 110
                    background: Rectangle { color: parent.down ? "#b71c1c" : "#d32f2f"; radius: 6 }
                    contentItem: Text { text: parent.text; color: "white"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: {
                        root.cancelarCaptura()
                        if (popupConfirmar.apagarModelo) {
                            repositorio.apagarModelo(root.modeloId)
                            root.mensagem = ""
                        } else {
                            repositorio.apagarCodigo(root.modeloId, root.comando)
                            root.mensagem = "Código de " + root.rotulo(root.comando) + " apagado."
                        }
                        popupConfirmar.close()
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------
    // Conteúdo
    // ---------------------------------------------------------------
    Rectangle {
        anchors.fill: parent
        anchors.margins: 20
        color: "white"
        radius: 12
        border.color: "#e0e0e0"

        RowLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 16

            // ---------- Coluna da esquerda: lista de modelos ----------
            ColumnLayout {
                Layout.preferredWidth: 200
                Layout.maximumWidth: 200
                Layout.fillHeight: true
                spacing: 8

                Text { text: "Modelos de ar"; font.pixelSize: 16; font.bold: true; color: "#333333" }

                ListView {
                    id: listaModelos
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 4
                    model: root.modelos

                    delegate: Rectangle {
                        width: listaModelos.width
                        height: 48
                        radius: 8
                        color: modelData.id === root.modeloId ? "#e3f2fd" : (areaModelo.containsMouse ? "#f5f5f5" : "transparent")
                        border.color: modelData.id === root.modeloId ? "#90caf9" : "transparent"

                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: 10
                            anchors.rightMargin: 8
                            spacing: 2
                            Text {
                                text: modelData.nome
                                width: parent.width
                                elide: Text.ElideRight
                                font.pixelSize: 14; font.bold: true; color: "#1e293b"
                            }
                            Text {
                                text: (modelData.marca !== "" ? modelData.marca + " · " : "") + modelData.codigos + "/17 códigos"
                                width: parent.width
                                elide: Text.ElideRight
                                font.pixelSize: 11
                                color: modelData.codigos === 17 ? "#2e7d32" : "#64748b"
                            }
                        }
                        MouseArea {
                            id: areaModelo
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: root.selecionarModelo(modelData.id)
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 20
                        visible: root.modelos.length === 0
                        text: "Nenhum modelo ainda."
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12; color: "#94a3b8"
                    }
                }

                Button {
                    text: "➕ Novo modelo"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    background: Rectangle { color: parent.down ? "#d5e4f5" : "#e3f2fd"; border.color: "#90caf9"; radius: 8 }
                    contentItem: Text { text: parent.text; font.bold: true; color: "#1565c0"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: popupModelo.abrir(false)
                }

                Button {
                    text: "← Voltar para a lista"
                    flat: true
                    Layout.alignment: Qt.AlignHCenter
                    contentItem: Text { text: parent.text; font.pixelSize: 13; color: "#757575"; horizontalAlignment: Text.AlignHCenter }
                    onClicked: { root.cancelarCaptura(); stackView.pop() }
                }
            }

            Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: "#eeeeee" }

            // ---------- Coluna da direita: códigos do modelo ----------
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                Text {
                    anchors.centerIn: parent
                    visible: root.modeloId < 0
                    text: "Crie um modelo para começar.\nCada modelo guarda os códigos de um tipo de ar-condicionado."
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: 13; color: "#94a3b8"
                }

                ColumnLayout {
                    anchors.fill: parent
                    visible: root.modeloId >= 0
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Column {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                text: root.modeloAtual() ? root.modeloAtual().nome : ""
                                width: parent.width
                                elide: Text.ElideRight
                                font.pixelSize: 18; font.bold: true; color: "#333333"
                            }
                            Text {
                                property var m: root.modeloAtual()
                                text: !m ? "" : ((m.marca !== "" ? m.marca + " · " : "")
                                       + (m.salas === 0 ? "nenhuma sala usa este modelo"
                                          : m.salas === 1 ? "usado por 1 sala" : "usado por " + m.salas + " salas"))
                                font.pixelSize: 12; color: "#64748b"
                            }
                        }
                        Button {
                            text: "✏️"
                            Layout.preferredWidth: 36; Layout.preferredHeight: 36
                            ToolTip.visible: hovered; ToolTip.text: "Renomear"
                            onClicked: popupModelo.abrir(true)
                        }
                        Button {
                            text: "🗑️"
                            Layout.preferredWidth: 36; Layout.preferredHeight: 36
                            ToolTip.visible: hovered; ToolTip.text: "Apagar modelo"
                            onClicked: { popupConfirmar.apagarModelo = true; popupConfirmar.open() }
                        }
                    }

                    Text {
                        text: "Escolha o comando (verde = já capturado):"
                        font.pixelSize: 13; color: "#666666"
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 5
                        rowSpacing: 6
                        columnSpacing: 6

                        Repeater {
                            model: root.listaComandos
                            delegate: Rectangle {
                                property bool capturado: root.comandos[modelData] === true
                                property bool selecionado: root.comando === modelData
                                Layout.fillWidth: true
                                Layout.columnSpan: index < 2 ? (index === 0 ? 3 : 2) : 1
                                Layout.preferredHeight: 34
                                radius: 6
                                color: capturado ? (selecionado ? "#2e7d32" : "#e8f5e9") : (selecionado ? "#475569" : "#f1f5f9")
                                border.color: selecionado ? "#1e293b" : (capturado ? "#a5d6a7" : "#e2e8f0")
                                border.width: selecionado ? 2 : 1
                                Text {
                                    anchors.centerIn: parent
                                    text: root.rotulo(modelData) + (capturado ? "  ✓" : "")
                                    font.pixelSize: 13
                                    font.bold: parent.selecionado
                                    color: parent.selecionado ? "white" : (parent.capturado ? "#2e7d32" : "#475569")
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: !root.aguardando
                                    onClicked: { root.comando = modelData; root.mensagem = "" }
                                }
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 13
                        color: root.aguardando ? "#1565c0" : "#333333"
                        text: root.aguardando
                              ? "📡 Aponte o controle para o receptor da ponte e aperte o botão de " + root.rotulo(root.comando) + "…"
                              : "Selecionado: " + root.rotulo(root.comando)
                                + (root.comandos[root.comando] === true ? " (já capturado)" : " (ainda não capturado)")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Button {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 44
                            text: root.aguardando ? "Cancelar leitura"
                                  : (root.comandos[root.comando] === true ? "📡 Regravar código" : "📡 Ler código do controle")
                            font.bold: true
                            background: Rectangle { color: parent.down ? "#d5e4f5" : "#e3f2fd"; border.color: "#90caf9"; radius: 8 }
                            contentItem: Text { text: parent.text; font: parent.font; color: "#1565c0"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            onClicked: {
                                if (root.aguardando) {
                                    root.cancelarCaptura()
                                    root.mensagem = ""
                                    return
                                }
                                root.mensagem = ""
                                root.aguardando = true
                                receiveData.set_waited_capture(root.modeloId, root.comando)
                                sendData.require_ir_read()
                            }
                        }

                        Button {
                            Layout.preferredHeight: 44
                            text: "Apagar código"
                            enabled: !root.aguardando && root.comandos[root.comando] === true
                            opacity: enabled ? 1.0 : 0.5
                            flat: true
                            contentItem: Text { text: parent.text; font.pixelSize: 13; color: "#d32f2f"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            onClicked: { popupConfirmar.apagarModelo = false; popupConfirmar.open() }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: root.mensagem !== ""
                        text: root.mensagem
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                        color: root.mensagem.indexOf("⚠️") === 0 ? "#d32f2f" : "#2e7d32"
                    }
                }
            }
        }
    }
}
