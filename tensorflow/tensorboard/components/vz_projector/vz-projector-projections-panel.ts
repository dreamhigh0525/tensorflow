/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

import {DataSet, MetadataInfo, PCA_SAMPLE_DIM, Projection, SAMPLE_SIZE, State} from './data';
import * as vector from './vector';
import {Projector} from './vz-projector';
import {ProjectorInput} from './vz-projector-input';
// tslint:disable-next-line:no-unused-variable
import {PolymerElement, PolymerHTMLElement} from './vz-projector-util';

// tslint:disable-next-line
export let ProjectionsPanelPolymer = PolymerElement({
  is: 'vz-projector-projections-panel',
  properties: {
    is3d: {type: Boolean, observer: '_dimensionsObserver'},
    // PCA projection.
    pcaComponents: {type: Array, value: d3.range(0, 10)},
    pcaX: {type: Number, value: 0, observer: 'showPCAIfEnabled'},
    pcaY: {type: Number, value: 1, observer: 'showPCAIfEnabled'},
    pcaZ: {type: Number, value: 2, observer: 'showPCAIfEnabled'},
    // Custom projection.
    selectedSearchByMetadataOption: {
      type: String,
      observer: '_searchByMetadataOptionChanged'
    },
  }
});

type InputControlName = 'xLeft' | 'xRight' | 'yUp' | 'yDown';

type CentroidResult = {
  centroid?: number[]; numMatches?: number;
};

type Centroids = {
  [key: string]: number[]; xLeft: number[]; xRight: number[]; yUp: number[];
  yDown: number[];
};

/**
 * A polymer component which handles the projection tabs in the projector.
 */
export class ProjectionsPanel extends ProjectionsPanelPolymer {
  selectedSearchByMetadataOption: string;
  is3d: boolean;

  private projector: Projector;
  private currentProjection: Projection;
  private polymerChangesTriggerReprojection: boolean;

  private dataSet: DataSet;
  private originalDataSet: DataSet;
  private dim: number;

  /** T-SNE perplexity. Roughly how many neighbors each point influences. */
  private perplexity: number;
  /** T-SNE learning rate. */
  private learningRate: number;

  private searchByMetadataOptions: string[];

  /** Centroids for custom projections. */
  private centroidValues: any;
  private centroids: Centroids;
  /** The centroid across all points. */
  private allCentroid: number[];

  /** Polymer properties. */
  // TODO(nsthorat): Move these to a separate view controller.
  public pcaX: number;
  public pcaY: number;
  public pcaZ: number;

  /** Polymer elements. */
  private dom: d3.Selection<any>;
  private runTsneButton: d3.Selection<HTMLButtonElement>;
  private stopTsneButton: d3.Selection<HTMLButtonElement>;
  private perplexitySlider: HTMLInputElement;
  private learningRateInput: HTMLInputElement;
  private zDropdown: d3.Selection<HTMLElement>;
  private iterationLabel: d3.Selection<HTMLElement>;

  initialize(projector: Projector) {
    this.polymerChangesTriggerReprojection = true;
    this.projector = projector;

    this.is3d = true;

    // Set up TSNE projections.
    this.perplexity = 30;
    this.learningRate = 10;

    // Setup Custom projections.
    this.centroidValues = {xLeft: null, xRight: null, yUp: null, yDown: null};
    this.clearCentroids();

    this.setupUIControls();
  }

  ready() {
    this.dom = d3.select(this);
    this.zDropdown = this.dom.select('#z-dropdown');
    this.runTsneButton = this.dom.select('.run-tsne');
    this.stopTsneButton = this.dom.select('.stop-tsne');
    this.perplexitySlider = this.$$('#perplexity-slider') as HTMLInputElement;
    this.learningRateInput =
        this.$$('#learning-rate-slider') as HTMLInputElement;
    this.iterationLabel = this.dom.select('.run-tsne-iter');
  }

  disablePolymerChangesTriggerReprojection() {
    this.polymerChangesTriggerReprojection = false;
  }

  enablePolymerChangesTriggerReprojection() {
    this.polymerChangesTriggerReprojection = true;
  }

  private updatePerplexityFromUIChange() {
    if (this.perplexitySlider) {
      this.perplexity = +this.perplexitySlider.value;
    }
    this.dom.select('.tsne-perplexity span').text(this.perplexity);
  }

  private updateLearningRateFromUIChange() {
    if (this.learningRateInput) {
      this.learningRate = Math.pow(10, +this.learningRateInput.value);
    }
    this.dom.select('.tsne-learning-rate span').text(this.learningRate);
  }

  private setupUIControls() {
    {
      const self = this;
      this.dom.selectAll('.ink-tab').on('click', function() {
        let id = this.getAttribute('data-tab');
        self.showTab(id);
      });
    }

    this.runTsneButton.on('click', () => this.runTSNE());
    this.stopTsneButton.on('click', () => this.dataSet.stopTSNE());

    this.perplexitySlider.value = this.perplexity.toString();
    this.perplexitySlider.addEventListener(
        'change', () => this.updatePerplexityFromUIChange());
    this.updatePerplexityFromUIChange();

    this.learningRateInput.addEventListener(
        'change', () => this.updateLearningRateFromUIChange());
    this.updateLearningRateFromUIChange();

    this.setupAllInputsInCustomTab();
    // TODO: figure out why `--paper-input-container-input` css mixin didn't
    // work.
    this.dom.selectAll('paper-dropdown-menu paper-input input')
      .style('font-size', '14px');
  }

  restoreUIFromBookmark(bookmark: State) {
    this.disablePolymerChangesTriggerReprojection();

    this.pcaX = bookmark.componentDimensions[0];
    this.pcaY = bookmark.componentDimensions[1];
    if (bookmark.componentDimensions.length === 3) {
      this.pcaZ = bookmark.componentDimensions[2];
    }
    if (this.perplexitySlider) {
      this.perplexitySlider.value = bookmark.tSNEPerplexity.toString();
    }
    if (this.learningRateInput) {
      this.learningRateInput.value = bookmark.tSNELearningRate.toString();
    }
    this.is3d = bookmark.is3d;

    this.setZDropdownEnabled(bookmark.componentDimensions.length === 3);
    this.updatePerplexityFromUIChange();
    this.updateLearningRateFromUIChange();
    if (this.iterationLabel) {
      this.iterationLabel.text(bookmark.tSNEIteration.toString());
    }
    this.showTab(bookmark.selectedProjection);

    this.enablePolymerChangesTriggerReprojection();
  }

  populateBookmarkFromUI(bookmark: State) {
    this.disablePolymerChangesTriggerReprojection();
    bookmark.componentDimensions = [this.pcaX, this.pcaY];
    if (this.is3d) {
      bookmark.componentDimensions.push(this.pcaZ);
    }
    bookmark.is3d = this.is3d;
    if (this.perplexitySlider) {
      bookmark.tSNEPerplexity = +this.perplexitySlider.value;
    }
    if (this.learningRateInput) {
      bookmark.tSNELearningRate = +this.learningRateInput.value;
    }
    this.enablePolymerChangesTriggerReprojection();
  }

  // This method is marked as public as it is used as the view method that
  // abstracts DOM manipulation so we can stub it in a test.
  // TODO(nsthorat): Move this to its own class as the glue between this class
  // and the DOM.
  public setZDropdownEnabled(enabled: boolean) {
    if (this.zDropdown) {
      this.zDropdown.attr('disabled', enabled ? null : true);
    }
  }

  dataSetUpdated(dataSet: DataSet, originalDataSet: DataSet, dim: number) {
    this.dataSet = dataSet;
    this.originalDataSet = originalDataSet;
    this.dim = dim;
    this.clearCentroids();

    this.dom.select('#tsne-sampling')
        .style('display', dataSet.points.length > SAMPLE_SIZE ? null : 'none');
    this.dom.select('#pca-sampling')
        .style('display', dataSet.dim[1] > PCA_SAMPLE_DIM ? null : 'none');
    this.showTab('pca');
  }

  _dimensionsObserver() {
    this.setZDropdownEnabled(this.is3d);
    this.beginProjection(this.currentProjection);
  }

  metadataChanged(metadata: MetadataInfo) {
    // Project by options for custom projections.
    let searchByMetadataIndex = -1;
    this.searchByMetadataOptions = metadata.stats.map((stats, i) => {
      // Make the default label by the first non-numeric column.
      if (!stats.isNumeric && searchByMetadataIndex === -1) {
        searchByMetadataIndex = i;
      }
      return stats.name;
    });
    this.selectedSearchByMetadataOption =
        this.searchByMetadataOptions[Math.max(0, searchByMetadataIndex)];
  }

  public showTab(id: Projection) {
    this.currentProjection = id;

    let tab = this.dom.select('.ink-tab[data-tab="' + id + '"]');
    this.dom.selectAll('.ink-tab').classed('active', false);
    tab.classed('active', true);
    this.dom.selectAll('.ink-panel-content').classed('active', false);
    this.dom.select('.ink-panel-content[data-panel="' + id + '"]')
        .classed('active', true);

    // In order for the projections panel to animate its height, we need to set
    // it explicitly.
    requestAnimationFrame(() => {
      this.style.height = this.$['main'].clientHeight + 'px';
    });

    this.beginProjection(id);
  }

  private beginProjection(projection: string) {
    if (this.polymerChangesTriggerReprojection) {
      if (projection === 'pca') {
        this.dataSet.stopTSNE();
        this.showPCA();
      } else if (projection === 'tsne') {
        this.showTSNE();
      } else if (projection === 'custom') {
        this.dataSet.stopTSNE();
        this.computeAllCentroids();
        this.reprojectCustom();
      }
    }
  }

  private showTSNE() {
    const dataSet = this.dataSet;
    if (dataSet == null) {
      return;
    }
    const accessors =
        dataSet.getPointAccessors('tsne', [0, 1, this.is3d ? 2 : null]);
    this.projector.setProjection('tsne', this.is3d ? 3 : 2, accessors);

    if (!this.dataSet.hasTSNERun) {
      this.runTSNE();
    } else {
      this.projector.notifyProjectionsUpdated();
    }
  }

  private runTSNE() {
    this.runTsneButton.attr('disabled', true);
    this.stopTsneButton.attr('disabled', null);
    this.dataSet.projectTSNE(
        this.perplexity, this.learningRate, this.is3d ? 3 : 2,
        (iteration: number) => {
          if (iteration != null) {
            this.iterationLabel.text(iteration);
            this.projector.notifyProjectionsUpdated();
          } else {
            this.runTsneButton.attr('disabled', null);
            this.stopTsneButton.attr('disabled', true);
          }
        });
  }

  // tslint:disable-next-line:no-unused-variable
  private showPCAIfEnabled() {
    if (this.polymerChangesTriggerReprojection) {
      this.showPCA();
    }
  }

  private showPCA() {
    if (this.dataSet == null) {
      return;
    }
    this.dataSet.projectPCA().then(() => {
      // Polymer properties are 1-based.
      const accessors = this.dataSet.getPointAccessors(
          'pca', [this.pcaX, this.pcaY, this.pcaZ]);

      this.projector.setProjection('pca', this.is3d ? 3 : 2, accessors);
    });
  }

  private reprojectCustom() {
    if (this.centroids == null || this.centroids.xLeft == null ||
        this.centroids.xRight == null || this.centroids.yUp == null ||
        this.centroids.yDown == null) {
      return;
    }
    const xDir = vector.sub(this.centroids.xRight, this.centroids.xLeft);
    this.dataSet.projectLinear(xDir, 'linear-x');

    const yDir = vector.sub(this.centroids.yUp, this.centroids.yDown);
    this.dataSet.projectLinear(yDir, 'linear-y');

    const accessors = this.dataSet.getPointAccessors('custom', ['x', 'y']);

    this.projector.setProjection('custom', 2, accessors);
  }

  clearCentroids(): void {
    this.centroids = {xLeft: null, xRight: null, yUp: null, yDown: null};
    this.allCentroid = null;
  }

  _searchByMetadataOptionChanged(newVal: string, oldVal: string) {
    if (this.currentProjection === 'custom') {
      this.computeAllCentroids();
      this.reprojectCustom();
    }
  }

  private setupAllInputsInCustomTab() {
    this.setupInputUIInCustomTab('xLeft');
    this.setupInputUIInCustomTab('xRight');
    this.setupInputUIInCustomTab('yUp');
    this.setupInputUIInCustomTab('yDown');
  }

  private computeAllCentroids() {
    this.computeCentroid('xLeft');
    this.computeCentroid('xRight');
    this.computeCentroid('yUp');
    this.computeCentroid('yDown');
  }

  private computeCentroid(name: InputControlName) {
    let input = this.querySelector('#' + name) as ProjectorInput;
    let value = input.getValue();
    let inRegexMode = input.getInRegexMode();

    if (value == null) {
      return;
    }
    let result = this.getCentroid(value, inRegexMode);
    if (result.numMatches === 0) {
      input.message = '0 matches. Using a random vector.';
      result.centroid = vector.rn(this.dim);
    } else {
      input.message = `${result.numMatches} matches.`;
    }
    this.centroids[name] = result.centroid;
    this.centroidValues[name] = value;
  }

  private setupInputUIInCustomTab(name: InputControlName) {
    let input = this.querySelector('#' + name) as ProjectorInput;
    // Setup the input text.
    input.onInputChanged((input, inRegexMode) => {
      this.computeCentroid(name);
      this.reprojectCustom();
    });
  }

  private getCentroid(pattern: string, inRegexMode: boolean): CentroidResult {
    if (pattern == null || pattern === '') {
      return {numMatches: 0};
    }
    // Search by the original dataset since we often want to filter and project
    // only the nearest neighbors of A onto B-C where B and C are not nearest
    // neighbors of A.
    let accessor = (i: number) => this.originalDataSet.points[i].vector;
    let r = this.originalDataSet.query(
        pattern, inRegexMode, this.selectedSearchByMetadataOption);
    return {centroid: vector.centroid(r, accessor), numMatches: r.length};
  }

  getPcaSampledDim() {
    return PCA_SAMPLE_DIM.toLocaleString();
  }

  getTsneSampleSize() {
    return SAMPLE_SIZE.toLocaleString();
  }

  _addOne(value: number) {
    return value + 1;
  }
}

document.registerElement(ProjectionsPanel.prototype.is, ProjectionsPanel);
