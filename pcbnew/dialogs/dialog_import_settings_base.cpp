///////////////////////////////////////////////////////////////////////////
// C++ code generated with wxFormBuilder (version Oct 26 2018)
// http://www.wxformbuilder.org/
//
// PLEASE DO *NOT* EDIT THIS FILE!
///////////////////////////////////////////////////////////////////////////

#include "dialog_import_settings_base.h"

///////////////////////////////////////////////////////////////////////////

DIALOG_IMPORT_SETTINGS_BASE::DIALOG_IMPORT_SETTINGS_BASE( wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos, const wxSize& size, long style ) : DIALOG_SHIM( parent, id, title, pos, size, style )
{
	this->SetSizeHints( wxSize( -1,-1 ), wxDefaultSize );

	m_MainSizer = new wxBoxSizer( wxVERTICAL );

	wxBoxSizer* bUpperSizer;
	bUpperSizer = new wxBoxSizer( wxHORIZONTAL );

	wxStaticText* importFromLabel;
	importFromLabel = new wxStaticText( this, wxID_ANY, _("Import from:"), wxDefaultPosition, wxDefaultSize, 0 );
	importFromLabel->Wrap( -1 );
	bUpperSizer->Add( importFromLabel, 0, wxALL|wxALIGN_CENTER_VERTICAL, 5 );

	m_filePathCtrl = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0 );
	m_filePathCtrl->SetToolTip( _("Target directory for plot files. Can be absolute or relative to the board file location.") );
	m_filePathCtrl->SetMinSize( wxSize( 300,-1 ) );

	bUpperSizer->Add( m_filePathCtrl, 1, wxEXPAND|wxTOP|wxBOTTOM, 5 );

	m_browseButton = new wxBitmapButton( this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxDefaultSize, wxBU_AUTODRAW|0 );
	m_browseButton->SetMinSize( wxSize( 29,29 ) );

	bUpperSizer->Add( m_browseButton, 0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 5 );


	m_MainSizer->Add( bUpperSizer, 0, wxEXPAND|wxTOP|wxRIGHT|wxLEFT, 5 );

	wxBoxSizer* bMiddleSizer;
	bMiddleSizer = new wxBoxSizer( wxHORIZONTAL );

	wxBoxSizer* bLeftCol;
	bLeftCol = new wxBoxSizer( wxVERTICAL );

	wxStaticText* importLabel;
	importLabel = new wxStaticText( this, wxID_ANY, _("Import:"), wxDefaultPosition, wxDefaultSize, 0 );
	importLabel->Wrap( -1 );
	bLeftCol->Add( importLabel, 0, wxTOP|wxBOTTOM|wxRIGHT, 5 );

	m_LayersOpt = new wxCheckBox( this, wxID_ANY, _("Layers setup"), wxDefaultPosition, wxDefaultSize, 0 );
	bLeftCol->Add( m_LayersOpt, 0, wxBOTTOM|wxRIGHT|wxLEFT, 5 );

	m_TextAndGraphicsOpt = new wxCheckBox( this, wxID_ANY, _("Text and graphics default properties"), wxDefaultPosition, wxDefaultSize, 0 );
	bLeftCol->Add( m_TextAndGraphicsOpt, 0, wxBOTTOM|wxRIGHT|wxLEFT, 5 );

	m_ConstraintsOpt = new wxCheckBox( this, wxID_ANY, _("Design rules"), wxDefaultPosition, wxDefaultSize, 0 );
	bLeftCol->Add( m_ConstraintsOpt, 0, wxBOTTOM|wxRIGHT|wxLEFT, 5 );

	m_TracksAndViasOpt = new wxCheckBox( this, wxID_ANY, _("Predefined track and via dimensions"), wxDefaultPosition, wxDefaultSize, 0 );
	bLeftCol->Add( m_TracksAndViasOpt, 0, wxBOTTOM|wxRIGHT|wxLEFT, 5 );

	m_MaskAndPasteOpt = new wxCheckBox( this, wxID_ANY, _("Solder mask/paste defaults"), wxDefaultPosition, wxDefaultSize, 0 );
	bLeftCol->Add( m_MaskAndPasteOpt, 0, wxBOTTOM|wxRIGHT|wxLEFT, 5 );

	m_SeveritiesOpt = new wxCheckBox( this, wxID_ANY, _("Violation severities"), wxDefaultPosition, wxDefaultSize, 0 );
	bLeftCol->Add( m_SeveritiesOpt, 0, wxRIGHT|wxLEFT, 5 );

	m_NetclassesOpt = new wxCheckBox( this, wxID_ANY, _("Net classes"), wxDefaultPosition, wxDefaultSize, 0 );
	bLeftCol->Add( m_NetclassesOpt, 0, wxALL, 5 );


	bMiddleSizer->Add( bLeftCol, 0, wxEXPAND|wxRIGHT|wxLEFT, 10 );

	wxBoxSizer* bRightCol;
	bRightCol = new wxBoxSizer( wxVERTICAL );

	m_selectAllButton = new wxButton( this, wxID_ANY, _("Select All"), wxDefaultPosition, wxDefaultSize, 0 );
	bRightCol->Add( m_selectAllButton, 1, wxALIGN_CENTER_VERTICAL|wxALL|wxALIGN_RIGHT, 10 );


	bMiddleSizer->Add( bRightCol, 1, wxEXPAND|wxALL, 20 );


	m_MainSizer->Add( bMiddleSizer, 1, wxEXPAND|wxBOTTOM, 5 );

	m_staticline1 = new wxStaticLine( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_HORIZONTAL );
	m_MainSizer->Add( m_staticline1, 0, wxEXPAND|wxTOP|wxRIGHT|wxLEFT, 5 );

	m_buttonsSizer = new wxBoxSizer( wxHORIZONTAL );

	m_sdbSizer1 = new wxStdDialogButtonSizer();
	m_sdbSizer1OK = new wxButton( this, wxID_OK );
	m_sdbSizer1->AddButton( m_sdbSizer1OK );
	m_sdbSizer1Cancel = new wxButton( this, wxID_CANCEL );
	m_sdbSizer1->AddButton( m_sdbSizer1Cancel );
	m_sdbSizer1->Realize();

	m_buttonsSizer->Add( m_sdbSizer1, 1, wxALL|wxEXPAND, 5 );


	m_MainSizer->Add( m_buttonsSizer, 0, wxEXPAND, 5 );


	this->SetSizer( m_MainSizer );
	this->Layout();
	m_MainSizer->Fit( this );

	this->Centre( wxBOTH );

	// Connect Events
	m_browseButton->Connect( wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnBrowseClicked ), NULL, this );
	m_LayersOpt->Connect( wxEVT_COMMAND_CHECKBOX_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnCheckboxClicked ), NULL, this );
	m_TextAndGraphicsOpt->Connect( wxEVT_COMMAND_CHECKBOX_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnCheckboxClicked ), NULL, this );
	m_ConstraintsOpt->Connect( wxEVT_COMMAND_CHECKBOX_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnCheckboxClicked ), NULL, this );
	m_TracksAndViasOpt->Connect( wxEVT_COMMAND_CHECKBOX_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnCheckboxClicked ), NULL, this );
	m_MaskAndPasteOpt->Connect( wxEVT_COMMAND_CHECKBOX_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnCheckboxClicked ), NULL, this );
	m_SeveritiesOpt->Connect( wxEVT_COMMAND_CHECKBOX_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnCheckboxClicked ), NULL, this );
	m_NetclassesOpt->Connect( wxEVT_COMMAND_CHECKBOX_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnCheckboxClicked ), NULL, this );
	m_selectAllButton->Connect( wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnSelectAll ), NULL, this );
}

DIALOG_IMPORT_SETTINGS_BASE::~DIALOG_IMPORT_SETTINGS_BASE()
{
	// Disconnect Events
	m_browseButton->Disconnect( wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnBrowseClicked ), NULL, this );
	m_LayersOpt->Disconnect( wxEVT_COMMAND_CHECKBOX_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnCheckboxClicked ), NULL, this );
	m_TextAndGraphicsOpt->Disconnect( wxEVT_COMMAND_CHECKBOX_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnCheckboxClicked ), NULL, this );
	m_ConstraintsOpt->Disconnect( wxEVT_COMMAND_CHECKBOX_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnCheckboxClicked ), NULL, this );
	m_TracksAndViasOpt->Disconnect( wxEVT_COMMAND_CHECKBOX_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnCheckboxClicked ), NULL, this );
	m_MaskAndPasteOpt->Disconnect( wxEVT_COMMAND_CHECKBOX_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnCheckboxClicked ), NULL, this );
	m_SeveritiesOpt->Disconnect( wxEVT_COMMAND_CHECKBOX_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnCheckboxClicked ), NULL, this );
	m_NetclassesOpt->Disconnect( wxEVT_COMMAND_CHECKBOX_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnCheckboxClicked ), NULL, this );
	m_selectAllButton->Disconnect( wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler( DIALOG_IMPORT_SETTINGS_BASE::OnSelectAll ), NULL, this );

}
