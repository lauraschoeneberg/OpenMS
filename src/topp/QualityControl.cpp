// --------------------------------------------------------------------------
//                   OpenMS -- Open-Source Mass Spectrometry
// --------------------------------------------------------------------------
// Copyright The OpenMS Team -- Eberhard Karls University Tuebingen,
// ETH Zurich, and Freie Universitaet Berlin 2002-2018.
//
// This software is released under a three-clause BSD license:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of any author or any participating institution
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
// For a full list of authors, refer to the file AUTHORS.
// --------------------------------------------------------------------------
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL ANY OF THE AUTHORS OR THE CONTRIBUTING
// INSTITUTIONS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// --------------------------------------------------------------------------
// $Maintainer: Chris Bielow $
// $Authors: Tom Waschischeck $
// --------------------------------------------------------------------------

#include <OpenMS/APPLICATIONS/TOPPBase.h>

#include <OpenMS/ANALYSIS/ID/IDConflictResolverAlgorithm.h>
#include <OpenMS/CONCEPT/Exception.h>
#include <OpenMS/FORMAT/ConsensusXMLFile.h>
#include <OpenMS/FORMAT/FileHandler.h>
#include <OpenMS/FORMAT/IdXMLFile.h>
#include <OpenMS/FORMAT/FASTAFile.h>
#include <OpenMS/FORMAT/FeatureXMLFile.h>
#include <OpenMS/FORMAT/FileTypes.h>
#include <OpenMS/FORMAT/MzIdentMLFile.h>
#include <OpenMS/FORMAT/MzMLFile.h>
#include <OpenMS/FORMAT/MzTabFile.h>
#include <OpenMS/FORMAT/TransformationXMLFile.h>
#include <OpenMS/KERNEL/MSExperiment.h>
#include <OpenMS/METADATA/PeptideIdentification.h>
#include <OpenMS/QC/Contaminants.h>
#include <OpenMS/QC/FragmentMassError.h>
#include <OpenMS/QC/MissedCleavages.h>
#include <OpenMS/QC/Ms2IdentificationRate.h>
#include <OpenMS/QC/MzCalibration.h>
#include <OpenMS/QC/QCBase.h>
#include <OpenMS/QC/RTAlignment.h>
#include <OpenMS/QC/TIC.h>
#include <OpenMS/QC/TopNoverRT.h>
#include <cstdio>

using namespace OpenMS;
using namespace std;

//-------------------------------------------------------------
// Doxygen docu
//-------------------------------------------------------------
// We do not want this class to show up in the docu:
/// @cond TOPPCLASSES


class TOPPQualityControl : public TOPPBase
{
public:
  TOPPQualityControl() : TOPPBase("QualityControl", "Does quality control for various input file types.", false)
  {
  }
protected:
  // this function will be used to register the tool parameters
  // it gets automatically called on tool execution
  void registerOptionsAndFlags_() override
  {
    registerInputFile_("in_cm", "<file>", "", "ConsensusXML input, generated by FeatureLinker.", true);
    setValidFormats_("in_cm", {"consensusXML"});
    registerInputFileList_("in_raw", "<file>", {}, "MzML input (after InternalCalibration, if available)", false);
    setValidFormats_("in_raw", {"mzML"});
    registerInputFileList_("in_postFDR", "<file>", {}, "featureXML input", false);
    setValidFormats_("in_postFDR", {"featureXML"});
    registerTOPPSubsection_("FragmentMassError", "test");
    registerStringOption_("FragmentMassError:unit", "<unit>", "auto", "Unit for tolerance. auto: information from FeatureXML", false);
    setValidStrings_("FragmentMassError:unit", std::vector<String>(FragmentMassError::names_of_toleranceUnit, FragmentMassError::names_of_toleranceUnit + (int)FragmentMassError::ToleranceUnit::SIZE_OF_TOLERANCEUNIT));
    registerDoubleOption_("FragmentMassError:tolerance", "<double>", 20, "Search window for matching peaks in two spectra", false);
    registerInputFile_("in_contaminants", "<file>", "", "Contaminant database input", false);
    setValidFormats_("in_contaminants", {"fasta"});
    registerInputFileList_("in_trafo", "<file>", {}, "trafoXML input", false);
    setValidFormats_("in_trafo", {"trafoXML"});
    registerTOPPSubsection_("MS2_id_rate", "test");
    registerFlag_("MS2_id_rate:force_no_fdr", "Forces the metric to run if FDR was not made, accept all pep_ids as target hits.", false);
    registerOutputFile_("out", "<file>", "", "mzTab with qc information", true);
    setValidFormats_("out", {"mzTab"});
    registerOutputFile_("out_cm", "<file>", "", "ConsensusXML with qc information", false);
    setValidFormats_("out_cm", {"consensusXML"});
    registerOutputFileList_("out_feat", "<file>", {}, "FeatureXML with qc information", false);
    setValidFormats_("out_feat", {"featureXML"});
    //TODO get ProteinQuantifier output for PRT section

  }

  // function tests if a metric has the required input files
  // gives a warning with the name of the metric that can not be performed
  bool isRunnable_(const QCBase* m, const OpenMS::QCBase::Status& s) const
  {
    if (s.isSuperSetOf(m->requires())) return true;

    for (Size i = 0; i < (UInt64)QCBase::Requires::SIZE_OF_REQUIRES; ++i)
    {
      if (m->requires().isSuperSetOf(QCBase::Status(QCBase::Requires(i))) && !s.isSuperSetOf(QCBase::Status(QCBase::Requires (i))) )
      {
        OPENMS_LOG_WARN << "Metric '" << m->getName() << "' cannot run because input data '" << QCBase::names_of_requires[i] << "' is missing!\n";
      }
    }
    return false;
  }
  // the main_ function is called after all parameters are read
  ExitCodes main_(int, const char **) override
  {
    //-------------------------------------------------------------
    // parsing parameters
    //-------------------------------------------------------------
    //
    // Read input, check for same length and get that length
    QCBase::Status status;
    UInt64 number_exps(0);
    StringList in_raw = updateFileStatus_(status, number_exps, "in_raw", QCBase::Requires::RAWMZML);
    StringList in_postFDR = updateFileStatus_(status, number_exps, "in_postFDR", QCBase::Requires::POSTFDRFEAT);
    StringList in_trafo = updateFileStatus_(status, number_exps, "in_trafo", QCBase::Requires::TRAFOALIGN);

    // load databases and other single file inputs
    String in_contaminants = getStringOption_("in_contaminants");
    FASTAFile fasta_file;
    vector<FASTAFile::FASTAEntry> contaminants;
    if (!in_contaminants.empty())
    {
      fasta_file.load(in_contaminants, contaminants);
      status |= QCBase::Requires::CONTAMINANTS;
    }
    ConsensusMap cmap;
    String in_cm = getStringOption_("in_cm");
    ConsensusXMLFile().load(in_cm, cmap);
    //-------------------------------------------------------------
    // Build the map to later find the original PepID in given ConsensusMap.
    //-------------------------------------------------------------
    map<String, PeptideIdentification*> map_to_id;
    for (Size i = 0; i < cmap.size(); ++i)
    {
      fillPepIDMap_(map_to_id, cmap[i].getPeptideIdentifications(), i);
    }
    fillPepIDMap_(map_to_id, cmap.getUnassignedPeptideIdentifications(), -1);
    //-------------------------------------------------------------
    // Build a map to associate newly created PepIDs to the correct ProteinID in CMap
    //-------------------------------------------------------------
    map<StringList, String> map_to_identifier;
    for (ProteinIdentification& prot_id : cmap.getProteinIdentifications())
    {
      StringList files;
      prot_id.getPrimaryMSRunPath(files);
      const auto& it = map_to_identifier.find(files);
      if (it != map_to_identifier.end())
      {
        OPENMS_LOG_ERROR << "Multiple protein identifications with the same identifier in ConsensusXML. Check input!\n";
        return ILLEGAL_PARAMETERS;
      }
      map_to_identifier[files] = prot_id.getIdentifier();
    }

    // check flags
    bool fdr_flag = getFlag_("MS2_id_rate:force_no_fdr");
    double tolerance_value = getDoubleOption_("FragmentMassError:tolerance");

    auto it = std::find(FragmentMassError::names_of_toleranceUnit, FragmentMassError::names_of_toleranceUnit + (int)FragmentMassError::ToleranceUnit::SIZE_OF_TOLERANCEUNIT, getStringOption_("FragmentMassError:unit"));
    auto idx = std::distance(FragmentMassError::names_of_toleranceUnit, it);
    auto tolerance_unit = FragmentMassError::ToleranceUnit(idx);


    // Instantiate the QC metrics
    Contaminants qc_contaminants;
    FragmentMassError qc_frag_mass_err;
    MissedCleavages qc_missed_cleavages;
    Ms2IdentificationRate qc_ms2ir;
    MzCalibration qc_mz_calibration;
    RTAlignment qc_rt_alignment;
    TIC qc_tic;
    TopNoverRT qc_top_n_over_rt;

    // Loop through file lists
    vector<PeptideIdentification> all_new_upep_ids;
    for (Size i = 0; i < number_exps; ++i)
    {
      //-------------------------------------------------------------
      // reading input
      //-------------------------------------------------------------
      MzMLFile mzml_file;
      PeakMap exp;
      QCBase::SpectraMap spec_map;
      if (!in_raw.empty())
      {
        mzml_file.load(in_raw[i], exp);
        spec_map.calculateMap(exp);
      }

      FeatureXMLFile fxml_file;
      FeatureMap fmap;
      if (!in_postFDR.empty())
      {
        fxml_file.load(in_postFDR[i], fmap);
      }

      TransformationXMLFile trafo_file;
      TransformationDescription trafo_descr;
      if (!in_trafo.empty())
      {
        trafo_file.load(in_trafo[i], trafo_descr);
      }
      //-------------------------------------------------------------
      // calculations
      //-------------------------------------------------------------

      if (isRunnable_(&qc_contaminants, status))
      {
        qc_contaminants.compute(fmap, contaminants);
      }

      if (isRunnable_(&qc_frag_mass_err, status))
      {
        qc_frag_mass_err.compute(fmap, exp, spec_map, tolerance_unit, tolerance_value);
      }

      if (isRunnable_(&qc_missed_cleavages, status))
      {
        qc_missed_cleavages.compute(fmap);
      }

      if (isRunnable_(&qc_ms2ir, status))
      {
        qc_ms2ir.compute(fmap, exp, fdr_flag);
      }

      if (isRunnable_(&qc_mz_calibration, status))
      {
        qc_mz_calibration.compute(fmap, exp, spec_map);
      }

      if (isRunnable_(&qc_rt_alignment, status))
      {
        qc_rt_alignment.compute(fmap, trafo_descr);
      }

      if (isRunnable_(&qc_tic, status))
      {
        qc_tic.compute(exp);
      }

      if (isRunnable_(&qc_top_n_over_rt, status))
      {
        vector<PeptideIdentification> new_upep_ids = qc_top_n_over_rt.compute(exp, fmap);
        // get and set identifier for just calculated IDs via MS run path
        StringList unique_run_path;
        fmap.getPrimaryMSRunPath(unique_run_path);
        const auto& ptr_to_map_entry = map_to_identifier.find(unique_run_path);
        if (ptr_to_map_entry == map_to_identifier.end())
        {
          OPENMS_LOG_ERROR << "FeatureXML (MS run '" << unique_run_path << "') does not correspond to ConsensusXML (run not found). Check input!\n";
          return ILLEGAL_PARAMETERS;
        }
        for (PeptideIdentification& pep_id : new_upep_ids)
        {
          pep_id.setIdentifier(ptr_to_map_entry -> second);
        }
        // save the just calculated IDs
        // this is needed like this, because appending the unassigned PepIDs directly to the ConsensusMap would destroy the mapping
        all_new_upep_ids.insert(all_new_upep_ids.end(),new_upep_ids.begin(),new_upep_ids.end());
      }

      StringList out_feat = getStringList_("out_feat");
      if (!out_feat.empty())
      {
        FeatureXMLFile().store(out_feat[i], fmap);
      }
      //------------------------------------------------------------- 
      // Annotate calculated meta values from FeatureMap to given ConsensusMap
      //-------------------------------------------------------------

      // copy MetaValues of unassigned PepIDs
      copyPepIDMetaValues_(fmap.getUnassignedPeptideIdentifications(), map_to_id);

      // copy MetaValues of assigned PepIDs
      for (Feature& feature : fmap)
      {
        copyPepIDMetaValues_(feature.getPeptideIdentifications(), map_to_id);
      }

    }
    // mztab writer requires single PIs per CF
    IDConflictResolverAlgorithm::resolve(cmap);

    // add calculated new unassigned PeptideIdentifications
    cmap.getUnassignedPeptideIdentifications().insert(cmap.getUnassignedPeptideIdentifications().end(), all_new_upep_ids.begin(), all_new_upep_ids.end());

    //-------------------------------------------------------------
    // writing output
    //-------------------------------------------------------------
    String out_cm = getStringOption_("out_cm");
    if (!out_cm.empty())
    {
      ConsensusXMLFile().store(out_cm, cmap);
    }

    MzTab mztab = MzTab::exportConsensusMapToMzTab(cmap, in_cm, true, true, true, true);

    MzTabMetaData meta = mztab.getMetaData();
    // Adding TIC information to meta data
    const auto& tics = qc_tic.getResults();
    for (Size i = 0; i < tics.size(); ++i)
    {
      MzTabParameter tic;
      tic.setCVLabel("total ion current");
      tic.setAccession("MS:1000285");
      tic.setName("TIC_" + String(i + 1));
      String value("[");
      value += String(tics[i][0].getRT()) + ", " + String(tics[i][0].getIntensity());
      for (Size j = 1; j < tics[i].size(); ++j)
      {
        value += ", " + String(tics[i][j].getRT()) + ", " + String(tics[i][j].getIntensity());
      }
      value += "]";
      tic.setValue(value);
      meta.custom[meta.custom.size()] = tic;
    }
    // Adding MS2_ID_Rate to meta data
    const auto& ms2_irs = qc_ms2ir.getResults();
    for (Size i = 0; i < ms2_irs.size(); ++i)
    {
      MzTabParameter ms2_ir;
      ms2_ir.setCVLabel("MS2 identification rate");
      ms2_ir.setAccession("null");
      ms2_ir.setName("MS2_ID_Rate_" + String(i + 1));
      ms2_ir.setValue(String(100 * ms2_irs[i].identification_rate));
      meta.custom[meta.custom.size()] = ms2_ir;
    }
    mztab.setMetaData(meta);


    MzTabFile mztab_out;
    mztab_out.store(getStringOption_("out"), mztab);
    return EXECUTION_OK;
  }

private:
  StringList updateFileStatus_(QCBase::Status& status, UInt64& number_exps, const String& port, const QCBase::Requires& req)
  {
    // since files are optional, leave function if none are provided by the user
    StringList files = getStringList_(port);
    if (!files.empty())
    {
      if (number_exps == 0) number_exps = files.size(); // Number of experiments is determined from first non empty file list.
      if (number_exps != files.size()) // exit if any file list has different length
      {
        throw(Exception::InvalidParameter(__FILE__, __LINE__, OPENMS_PRETTY_FUNCTION, port + ": invalid number of files. Expected were " + number_exps + ".\n"));
      }
      status |= req;
    }
    return files;
  }

  // templated function to copy all meta values from one object to another
  template <class FROM, class TO>
  //TODO get a MetaValue list to copy only those that have been set
  void copyMetaValues_(const FROM& from, TO& to) const
  {
    vector<String> keys;
    from.getKeys(keys);
    for (String& key : keys)
    {
      to.setMetaValue(key, from.getMetaValue(key));
    }
  }

  void copyPepIDMetaValues_(const vector<PeptideIdentification>& pep_ids, const map<String, PeptideIdentification*>& map_to_id) const
  {
    for (const PeptideIdentification& ref_pep_id : pep_ids)
    {
      // for empty PIs which were created by a metric
      if (ref_pep_id.getHits().empty()) continue;

      if (!ref_pep_id.metaValueExists("UID")) // PepID doesn't has ID, needs to have MetaValue
      {
        throw(Exception::InvalidParameter(__FILE__, __LINE__, OPENMS_PRETTY_FUNCTION, "No unique ID at peptideidentifications found. Please run PeptideIndexer with '-addUID'.\n"));
      }
      PeptideIdentification& pep_id = *(map_to_id.at(ref_pep_id.getMetaValue("UID"))); 

      // copy all MetaValues that are at PepID level
      copyMetaValues_(ref_pep_id, pep_id);

      // copy all MetaValues that are at Hit level
      copyMetaValues_(ref_pep_id.getHits()[0], pep_id.getHits()[0]);
    }
  }

  void fillPepIDMap_(map<String, PeptideIdentification*>& map_to_id, vector<PeptideIdentification>& pep_ids, const int group_id) const
  {
    for ( PeptideIdentification& pep_id : pep_ids)
    {
      if (!pep_id.metaValueExists("UID")) // PepID doesn't has ID, needs to have MetaValue
      {
        throw(Exception::InvalidParameter(__FILE__, __LINE__, OPENMS_PRETTY_FUNCTION, "No unique ID at peptideidentifications found. Please run PeptideIndexer with '-addUID'.\n"));
      }
      pep_id.setMetaValue("cf_id", group_id);
      map_to_id[pep_id.getMetaValue("UID")] = &pep_id;
    }
  }
};

// the actual main function needed to create an executable
int main(int argc, const char ** argv)
{
  TOPPQualityControl tool;
  return tool.main(argc, argv);
}

/// @endcond
