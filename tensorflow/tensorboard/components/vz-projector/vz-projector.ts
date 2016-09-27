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

import {ColorOption, DataPoint, DataSet, PCA_SAMPLE_DIM, Projection, SAMPLE_SIZE, State} from './data';
import {DataProvider, getDataProvider} from './data-loader';
import * as knn from './knn';
import {Mode, ScatterPlotWebGL} from './scatterPlotWebGL';
import {ScatterPlotWebGLVisualizer3DLabels} from './scatterPlotWebGLVisualizer3DLabels';
import {ScatterPlotWebGLVisualizerCanvasLabels} from './scatterPlotWebGLVisualizerCanvasLabels';
import {ScatterPlotWebGLVisualizerSprites} from './scatterPlotWebGLVisualizerSprites';
import {ScatterPlotWebGLVisualizerTraces} from './scatterPlotWebGLVisualizerTraces';
import * as vector from './vector';
import {BookmarkPanel} from './vz-projector-bookmark-panel';
import {DataPanel} from './vz-projector-data-panel';
// tslint:disable-next-line:no-unused-variable
import {PolymerElement, PolymerHTMLElement} from './vz-projector-util';


/** T-SNE perplexity. Roughly how many neighbors each point influences. */
let perplexity: number = 30;
/** T-SNE learning rate. */
let learningRate: number = 10;
/** Number of dimensions for the scatter plot. */
let dimension = 3;
/** Number of nearest neighbors to highlight around the selected point. */
let numNN = 100;

/** Highlight stroke color for the nearest neighbors. */
const NN_HIGHLIGHT_COLOR = '#FA6666';
/** Color to denote a missing value. */
const MISSING_VALUE_COLOR = 'black';
/** Highlight stroke color for the selected point */
const POINT_HIGHLIGHT_COLOR = '#760B4F';

/** Color scale for nearest neighbors. */
const NN_COLOR_SCALE =
    d3.scale.linear<string>()
        .domain([1, 0.7, 0.4])
        .range(['hsl(285, 80%, 40%)', 'hsl(0, 80%, 65%)', 'hsl(40, 70%, 60%)'])
        .clamp(true);

/** Text color used for error/important messages. */
const CALLOUT_COLOR = '#880E4F';

/**
 * The minimum number of dimensions the data should have to automatically
 * decide to normalize the data.
 */
const THRESHOLD_DIM_NORMALIZE = 50;

type Centroids = {
  [key: string]: number[]; xLeft: number[]; xRight: number[]; yUp: number[];
  yDown: number[];
};

export let ProjectorPolymer = PolymerElement({
  is: 'vz-projector',
  properties: {
    // Private.
    pcaComponents: {type: Array, value: d3.range(1, 11)},
    pcaX: {type: Number, value: 0, observer: 'showPCA'},
    pcaY: {type: Number, value: 1, observer: 'showPCA'},
    pcaZ: {type: Number, value: 2, observer: 'showPCA'},
    routePrefix: String,
    hasPcaZ: {type: Boolean, value: true},
    labelOption: {type: String, observer: '_labelOptionChanged'},
    colorOption: {type: Object, observer: '_colorOptionChanged'}
  }
});

export class Projector extends ProjectorPolymer {
  dataSet: DataSet;

  private dom: d3.Selection<any>;
  private pcaX: number;
  private pcaY: number;
  private pcaZ: number;
  private hasPcaZ: boolean;
  // The working subset of the data source's original data set.
  private currentDataSet: DataSet;
  private scatterPlot: ScatterPlotWebGL;
  private labels3D: boolean = false;
  private dim: number;
  private selectedDistance: (a: number[], b: number[]) => number;
  private highlightedPoints: {index: number, color: string}[];
  // The index of all selected points.
  private selectedPoints: number[];
  private centroidValues: any;
  private centroids: Centroids;
  /** The centroid across all points. */
  private allCentroid: number[];
  private dataProvider: DataProvider;
  private dataPanel: DataPanel;
  private bookmarkPanel: BookmarkPanel;
  private colorOption: ColorOption;
  private labelOption: string;
  private routePrefix: string;
  private selectedProjection: Projection = 'pca';
  private normalizeData: boolean;

  ready() {
    this.dataPanel = this.$['data-panel'] as DataPanel;
    // Get the data loader and initialize the data panel with it.
    getDataProvider(this.routePrefix, dataProvider => {
      this.dataProvider = dataProvider;
      this.dataPanel.initialize(this, dataProvider);
    });

    this.bookmarkPanel = this.$['bookmark-panel'] as BookmarkPanel;
    this.bookmarkPanel.initialize(this);

    // And select a default dataset.
    this.hasPcaZ = true;
    this.selectedDistance =
        this.normalizeData ? vector.cosDistNorm : vector.cosDist;
    this.highlightedPoints = [];
    this.selectedPoints = [];
    this.centroidValues = {xLeft: null, xRight: null, yUp: null, yDown: null};
    this.centroids = {xLeft: null, xRight: null, yUp: null, yDown: null};
    // Dynamically creating elements inside .nn-list.
    this.scopeSubtree(this.$$('.nn-list'), true);
    this.dom = d3.select(this);
    // Sets up all the UI.
    this.setupUIControls();
  }

  _labelOptionChanged() {
    let labelAccessor = (i: number): string => {
      return this.points[i].metadata[this.labelOption] as string;
    };
    this.scatterPlot.setLabelAccessor(labelAccessor);
  }

  _colorOptionChanged() {
    let colorMap = this.colorOption.map;
    if (colorMap == null) {
      this.scatterPlot.setColorAccessor(null);
      return;
    };
    let colors = (i: number) => {
      let value = this.points[i].metadata[this.colorOption.name];
      if (value == null) {
        return MISSING_VALUE_COLOR;
      }
      return colorMap(value);
    };
    this.scatterPlot.setColorAccessor(colors);
  }

  setNormalizeData(normalizeData: boolean) {
    this.normalizeData = normalizeData;
    // Assign the proper distance metric depending on whether the data was
    // normalized or not.
    if (this.selectedDistance !== vector.dist) {
      this.selectedDistance =
          this.normalizeData ? vector.cosDistNorm : vector.cosDist;
    }
    this.setCurrentDataSet(this.dataSet.getSubset());
  }

  updateDataSet(ds: DataSet) {
    this.dataSet = ds;
    if (this.scatterPlot == null || this.dataSet == null) {
      // We are not ready yet.
      return;
    }
    this.normalizeData = this.dataSet.dim[1] >= THRESHOLD_DIM_NORMALIZE;
    this.dataPanel.setNormalizeData(this.normalizeData);
    this.setCurrentDataSet(this.dataSet.getSubset());
    this.dom.select('.reset-filter').style('display', 'none');
    // Regexp inputs.
    this.setupInput('xLeft');
    this.setupInput('xRight');
    this.setupInput('yUp');
    this.setupInput('yDown');

    // Set the container to a fixed height, otherwise in Colab the
    // height can grow indefinitely.
    let container = this.dom.select('#container');
    container.style('height', container.property('clientHeight') + 'px');
  }

  /**
   * Normalizes the distance so it can be visually encoded with color.
   * The normalization depends on the distance metric (cosine vs euclidean).
   */
  private normalizeDist(d: number, minDist: number): number {
    return this.selectedDistance === vector.dist ? minDist / d : 1 - d;
  }

  /** Normalizes and encodes the provided distance with color. */
  private dist2color(d: number, minDist: number): string {
    return NN_COLOR_SCALE(this.normalizeDist(d, minDist));
  }

  private setCurrentDataSet(ds: DataSet) {
    this.currentDataSet = ds;
    if (this.normalizeData) {
      this.currentDataSet.normalize();
    }
    this.scatterPlot.setDataSet(this.currentDataSet, this.dataSet.spriteImage);
    this.updateMenuButtons();
    this.dim = this.currentDataSet.dim[1];
    this.dom.select('span.numDataPoints').text(this.currentDataSet.dim[0]);
    this.dom.select('span.dim').text(this.currentDataSet.dim[1]);
    this.showTab('pca', true /* recreateScene */);
  }

  private setupInput(name: string) {
    let control = this.dom.select('.control.' + name);
    let info = control.select('.info');

    let updateInput = (value: string) => {
      if (value.trim() === '') {
        info.style('color', CALLOUT_COLOR).text('Enter a regex.');
        return;
      }
      let result = this.getCentroid(value);
      if (result.error) {
        info.style('color', CALLOUT_COLOR)
            .text('Invalid regex. Using a random vector.');
        result.centroid = vector.rn(this.dim);
      } else if (result.numMatches === 0) {
        info.style('color', CALLOUT_COLOR)
            .text('0 matches. Using a random vector.');
        result.centroid = vector.rn(this.dim);
      } else {
        info.style('color', null).text(`${result.numMatches} matches.`);
      }
      this.centroids[name] = result.centroid;
      this.centroidValues[name] = value;
    };
    let self = this;

    let input = control.select('input').on('input', function() {
      updateInput(this.value);
      self.showCustom();
    });
    this.allCentroid = null;
    // Init the control with the current input.
    updateInput((input.node() as HTMLInputElement).value);
  }

  private setupUIControls() {
    let self = this;
    // Global tabs
    this.dom.selectAll('.ink-tab').on('click', function() {
      let id = this.getAttribute('data-tab');
      self.showTab(id);
    });

    // Unknown why, but the polymer toggle button stops working
    // as soon as you do d3.select() on it.
    let tsneToggle = this.querySelector('#tsne-toggle') as HTMLInputElement;
    let zCheckbox = this.querySelector('#z-checkbox') as HTMLInputElement;

    // PCA controls.
    zCheckbox.addEventListener('change', () => {
      // Make sure tsne stays in the same dimension as PCA.
      dimension = this.hasPcaZ ? 3 : 2;
      tsneToggle.checked = this.hasPcaZ;
      this.showPCA(() => {
        this.scatterPlot.recreateScene();
      });
    });

    // TSNE controls.
    tsneToggle.addEventListener('change', () => {
      // Make sure PCA stays in the same dimension as tsne.
      this.hasPcaZ = tsneToggle.checked;
      dimension = tsneToggle.checked ? 3 : 2;
      if (this.scatterPlot) {
        this.showTSNE();
        this.scatterPlot.recreateScene();
      }
    });

    this.dom.select('.run-tsne').on('click', () => this.runTSNE());
    this.dom.select('.stop-tsne').on('click', () => {
      this.currentDataSet.stopTSNE();
    });

    let perplexityInput = this.dom.select('.tsne-perplexity input');
    let updatePerplexity = () => {
      perplexity = +perplexityInput.property('value');
      this.dom.select('.tsne-perplexity span').text(perplexity);
    };
    perplexityInput.property('value', perplexity).on('input', updatePerplexity);
    updatePerplexity();

    let learningRateInput = this.dom.select('.tsne-learning-rate input');
    let updateLearningRate = () => {
      let val = +learningRateInput.property('value');
      learningRate = Math.pow(10, val);
      this.dom.select('.tsne-learning-rate span').text(learningRate);
    };
    learningRateInput.property('value', 1).on('input', updateLearningRate);
    updateLearningRate();

    // Nearest neighbors controls.
    let numNNInput = this.dom.select('.num-nn input');
    let updateNumNN = () => {
      numNN = +numNNInput.property('value');
      this.dom.select('.num-nn span').text(numNN);
    };
    numNNInput.property('value', numNN).on('input', updateNumNN);
    updateNumNN();

    // View controls
    this.dom.select('.reset-zoom').on('click', () => {
      this.scatterPlot.resetZoom();
    });
    this.dom.select('.zoom-in').on('click', () => {
      this.scatterPlot.zoomStep(2);
    });
    this.dom.select('.zoom-out').on('click', () => {
      this.scatterPlot.zoomStep(0.5);
    });

    // Toolbar controls
    let searchBox = this.dom.select('.control.search-box');
    let searchBoxInfo = searchBox.select('.info');

    let searchByRegEx =
        (pattern: string): {error?: Error, indices: number[]} => {
          let regEx: RegExp;
          try {
            regEx = new RegExp(pattern, 'i');
          } catch (e) {
            return {error: e.message, indices: null};
          }
          let indices: number[] = [];
          for (let id = 0; id < this.points.length; ++id) {
            if (regEx.test('' + this.points[id].metadata['label'])) {
              indices.push(id);
            }
          }
          return {indices: indices};
        };

    // Called whenever the search text input changes.
    let searchInputChanged = (value: string) => {
      if (value.trim() === '') {
        searchBoxInfo.style('color', CALLOUT_COLOR).text('Enter a regex.');
        if (this.scatterPlot != null) {
          this.selectedPoints = [];
          this.selectionWasUpdated();
        }
        return;
      }
      let result = searchByRegEx(value);
      let indices = result.indices;
      if (result.error) {
        searchBoxInfo.style('color', CALLOUT_COLOR).text('Invalid regex.');
      }
      if (indices) {
        if (indices.length === 0) {
          searchBoxInfo.style('color', CALLOUT_COLOR).text(`0 matches.`);
        } else {
          searchBoxInfo.style('color', null).text(`${indices.length} matches.`);
          this.showTab('inspector');
          let neighbors = this.findNeighbors(indices[0]);
          this.selectedPoints = indices;
          this.updateInspectorPane(neighbors);
        }
        this.selectionWasUpdated();
      }
    };

    searchBox.select('input').on(
        'input', function() { searchInputChanged(this.value); });
    let searchButton = this.dom.select('.search');

    searchButton.on('click', () => {
      let mode = this.scatterPlot.getMode();
      this.scatterPlot.setMode(mode === Mode.SEARCH ? Mode.HOVER : Mode.SEARCH);
      if (this.scatterPlot.getMode() === Mode.HOVER) {
        this.selectedPoints = [];
        this.selectionWasUpdated();
      } else {
        searchInputChanged(searchBox.select('input').property('value'));
      }
      this.updateMenuButtons();
    });
    // Init the control with an empty input.
    searchInputChanged('');

    this.dom.select('.distance a.euclidean').on('click', function() {
      self.dom.selectAll('.distance a').classed('selected', false);
      d3.select(this).classed('selected', true);
      self.selectedDistance = vector.dist;
      if (self.selectedPoints.length > 0) {
        let neighbors = self.findNeighbors(self.selectedPoints[0]);
        self.updateInspectorPane(neighbors);
      }
    });

    this.dom.select('.distance a.cosine').on('click', function() {
      self.dom.selectAll('.distance a').classed('selected', false);
      d3.select(this).classed('selected', true);
      self.selectedDistance =
          this.normalizeData ? vector.cosDistNorm : vector.cosDist;
      if (self.selectedPoints.length > 0) {
        let neighbors = self.findNeighbors(self.selectedPoints[0]);
        self.updateInspectorPane(neighbors);
      }
    });

    let selectModeButton = this.dom.select('.selectMode');
    selectModeButton.on('click', () => {
      let mode = this.scatterPlot.getMode();
      this.scatterPlot.setMode(mode === Mode.SELECT ? Mode.HOVER : Mode.SELECT);
      this.updateMenuButtons();
    });

    let dayNightModeButton = this.dom.select('.nightDayMode');
    let modeIsNight = dayNightModeButton.classed('selected');
    dayNightModeButton.on('click', () => {
      modeIsNight = !modeIsNight;
      dayNightModeButton.classed('selected', modeIsNight);
      this.scatterPlot.setDayNightMode(modeIsNight);
    });

    let labels3DModeButton = this.dom.select('.labels3DMode');
    labels3DModeButton.on('click', () => {
      this.labels3D = !this.labels3D;
      this.createVisualizers();
      this.scatterPlot.recreateScene();
      this.scatterPlot.update();
      this.updateMenuButtons();
    });

    // Resize
    window.addEventListener('resize', () => {
      this.scatterPlot.resize();
    });

    // Canvas
    {
      this.scatterPlot = new ScatterPlotWebGL(
          this.getScatterContainer(),
          i => '' + this.points[i].metadata['label']);
      this.createVisualizers();
    }

    this.scatterPlot.onHover(hoveredIndex => {
      if (hoveredIndex == null) {
        this.highlightedPoints = [];
      } else {
        let point = this.points[hoveredIndex];
        this.dom.select('#hoverInfo').text(point.metadata['label']);
        this.highlightedPoints =
            [{index: hoveredIndex, color: POINT_HIGHLIGHT_COLOR}];
      }
      this.selectionWasUpdated();
    });

    this.scatterPlot.onSelection(
        selectedPoints => this.updateSelection(selectedPoints));

    this.scatterPlot.onCameraMove(
        (cameraPosition: THREE.Vector3, cameraTarget: THREE.Vector3) =>
            this.bookmarkPanel.clearStateSelection());

    // Selection controls
    this.dom.select('.set-filter').on('click', () => {
      let highlighted = this.selectedPoints;
      let highlightedOrig: number[] = highlighted.map(d => {
        return this.points[d].index;
      });
      this.setCurrentDataSet(this.dataSet.getSubset(highlightedOrig));
      this.dom.select('.reset-filter').style('display', null);
      this.selectedPoints = [];
      this.scatterPlot.recreateScene();
      this.selectionWasUpdated();
      this.updateIsolateButton();
    });

    this.dom.select('.reset-filter').on('click', () => {
      this.setCurrentDataSet(this.dataSet.getSubset(null));
      this.dom.select('.reset-filter').style('display', 'none');
    });

    this.dom.select('.clear-selection').on('click', () => {
      this.selectedPoints = [];
      this.scatterPlot.setMode(Mode.HOVER);
      this.scatterPlot.clickOnPoint(null);
      this.updateMenuButtons();
      this.selectionWasUpdated();
    });
  }

  private getScatterContainer(): d3.Selection<any> {
    return this.dom.select('#scatter');
  }

  private createVisualizers() {
    let scatterPlotWebGL = this.scatterPlot;
    scatterPlotWebGL.removeAllVisualizers();

    if (this.labels3D) {
      scatterPlotWebGL.addVisualizer(
          new ScatterPlotWebGLVisualizer3DLabels(scatterPlotWebGL));
    } else {
      scatterPlotWebGL.addVisualizer(
          new ScatterPlotWebGLVisualizerSprites(scatterPlotWebGL));

      scatterPlotWebGL.addVisualizer(
          new ScatterPlotWebGLVisualizerTraces(scatterPlotWebGL));

      scatterPlotWebGL.addVisualizer(new ScatterPlotWebGLVisualizerCanvasLabels(
          this.getScatterContainer()));
    }
  }

  private updateSelection(points: number[]) {
    // If no points are selected, unselect everything.
    if (!points.length) {
      this.selectedPoints = [];
      this.updateInspectorPane([]);
    } else {
      // Get the nearest neighbors of the first selected point and update the
      // UI accordingly.
      this.showTab('inspector');
      let neighbors = this.findNeighbors(points[0]);
      this.selectedPoints = [points[0]].concat(neighbors.map(n => n.index));
      this.updateInspectorPane(neighbors);
    }
    this.selectionWasUpdated();
  }

  private showPCA(callback?: () => void) {
    if (this.currentDataSet == null) {
      return;
    }
    this.selectedProjection = 'pca';
    this.currentDataSet.projectPCA().then(() => {
      this.scatterPlot.showTickLabels(false);
      let x = this.pcaX;
      let y = this.pcaY;
      let z = this.pcaZ;
      let hasZ = dimension === 3;
      this.scatterPlot.setPointAccessors(
          i => this.points[i].projections['pca-' + x],
          i => this.points[i].projections['pca-' + y],
          hasZ ? (i => this.points[i].projections['pca-' + z]) : null);
      this.scatterPlot.setAxisLabels('pca-' + x, 'pca-' + y);
      this.scatterPlot.update();
      if (callback) {
        callback();
      }
    });
  }

  private showTab(id: string, recreateScene = false) {
    let tab = this.dom.select('.ink-tab[data-tab="' + id + '"]');
    let pane =
        d3.select((tab.node() as HTMLElement).parentNode.parentNode.parentNode);
    pane.selectAll('.ink-tab').classed('active', false);
    tab.classed('active', true);
    pane.selectAll('.ink-panel-content').classed('active', false);
    pane.select('.ink-panel-content[data-panel="' + id + '"]')
        .classed('active', true);
    if (id === 'pca') {
      this.showPCA(() => {
        if (recreateScene) {
          this.scatterPlot.recreateScene();
        }
      });
    } else if (id === 'tsne') {
      this.showTSNE();
    } else if (id === 'custom') {
      this.showCustom();
    }
  }

  private showCustom() {
    this.selectedProjection = 'custom';
    this.scatterPlot.showTickLabels(true);
    let xDir = vector.sub(this.centroids.xRight, this.centroids.xLeft);
    this.currentDataSet.projectLinear(xDir, 'linear-x');

    let yDir = vector.sub(this.centroids.yUp, this.centroids.yDown);
    this.currentDataSet.projectLinear(yDir, 'linear-y');

    this.scatterPlot.setPointAccessors(
        i => this.points[i].projections['linear-x'],
        i => this.points[i].projections['linear-y'], null);

    let xLabel = this.centroidValues.xLeft + ' → ' + this.centroidValues.xRight;
    let yLabel = this.centroidValues.yUp + ' → ' + this.centroidValues.yDown;
    this.scatterPlot.setAxisLabels(xLabel, yLabel);
    this.scatterPlot.update();
    this.scatterPlot.recreateScene();
  }

  private get points() { return this.currentDataSet.points; }

  private showTSNE() {
    this.selectedProjection = 'tsne';
    this.scatterPlot.showTickLabels(false);
    this.scatterPlot.setPointAccessors(
        i => this.points[i].projections['tsne-0'],
        i => this.points[i].projections['tsne-1'],
        dimension === 3 ? (i => this.points[i].projections['tsne-2']) : null);
    this.scatterPlot.setAxisLabels('tsne-0', 'tsne-1');
    if (!this.currentDataSet.hasTSNERun) {
      this.runTSNE();
    } else {
      this.scatterPlot.update();
    }
  }

  private runTSNE() {
    this.currentDataSet.projectTSNE(
        perplexity, learningRate, dimension, (iteration: number) => {
          if (iteration != null) {
            this.dom.select('.run-tsne-iter').text(iteration);
            this.scatterPlot.update();
          }
        });
  }

  // Updates the displayed metadata for the selected point.
  private updateMetadata() {
    let metadataContainerElement = this.dom.select('.ink-panel-metadata');
    metadataContainerElement.selectAll('*').remove();

    let point = this.points[this.selectedPoints[0]];
    this.dom.select('.ink-panel-metadata-container')
        .style('display', point != null ? '' : 'none');

    if (point == null) {
      return;
    }

    for (let metadataKey in point.metadata) {
      if (!point.metadata.hasOwnProperty(metadataKey)) {
        continue;
      }
      let rowElement = document.createElement('div');
      rowElement.className = 'ink-panel-metadata-row vz-projector';

      let keyElement = document.createElement('div');
      keyElement.className = 'ink-panel-metadata-key vz-projector';
      keyElement.textContent = metadataKey;

      let valueElement = document.createElement('div');
      valueElement.className = 'ink-panel-metadata-value vz-projector';
      valueElement.textContent = '' + point.metadata[metadataKey];

      rowElement.appendChild(keyElement);
      rowElement.appendChild(valueElement);

      metadataContainerElement.append(function() {
        return this.appendChild(rowElement);
      });
    }
  }

  private selectionWasUpdated() {
    this.dom.select('#hoverInfo')
        .text(`Selected ${this.selectedPoints.length} points`);
    let allPoints =
        this.highlightedPoints.map(x => x.index).concat(this.selectedPoints);
    let stroke = (i: number) => {
      return i < this.highlightedPoints.length ?
          this.highlightedPoints[i].color :
          NN_HIGHLIGHT_COLOR;
    };
    let favor = (i: number) => {
      return i === 0 || (i < this.highlightedPoints.length ? false : true);
    };
    this.scatterPlot.highlightPoints(allPoints, stroke, favor);
    this.updateIsolateButton();
  }

  private updateMenuButtons() {
    let searchBox = this.dom.select('.control.search-box');
    this.dom.select('.search').classed(
        'selected', this.scatterPlot.getMode() === Mode.SEARCH);
    let searchMode = this.scatterPlot.getMode() === Mode.SEARCH;
    this.dom.select('.control.search-box')
        .style('width', searchMode ? '110px' : null)
        .style('margin-right', searchMode ? '10px' : null);
    (searchBox.select('input').node() as HTMLInputElement).focus();
    this.dom.select('.selectMode')
        .classed('selected', this.scatterPlot.getMode() === Mode.SELECT);
    this.dom.select('.labels3DMode').classed('selected', this.labels3D);
  }

  /**
   * Finds the nearest neighbors of the currently selected point using the
   * currently selected distance method.
   */
  private findNeighbors(pointIndex: number): knn.NearestEntry[] {
    // Find the nearest neighbors of a particular point.
    let neighbors = knn.findKNNofPoint(
        this.points, pointIndex, numNN, (d => d.vector), this.selectedDistance);
    let result = neighbors.slice(0, numNN);
    return result;
  }

  /** Updates the nearest neighbors list in the inspector. */
  private updateInspectorPane(neighbors: knn.NearestEntry[]) {
    this.updateMetadata();
    let nnlist = this.dom.select('.nn-list');
    nnlist.html('');

    if (neighbors.length === 0) {
      return;
    }

    let minDist = neighbors.length > 0 ? neighbors[0].dist : 0;
    let n = nnlist.selectAll('.neighbor')
                .data(neighbors)
                .enter()
                .append('div')
                .attr('class', 'neighbor')
                .append('a')
                .attr('class', 'neighbor-link');

    n.append('span')
        .attr('class', 'label')
        .style('color', d => this.dist2color(d.dist, minDist))
        .text(d => this.points[d.index].metadata['label']);

    n.append('span').attr('class', 'value').text(d => d.dist.toFixed(2));

    let bar = n.append('div').attr('class', 'bar');

    bar.append('div')
        .attr('class', 'fill')
        .style('border-top-color', d => this.dist2color(d.dist, minDist))
        .style('width', d => this.normalizeDist(d.dist, minDist) * 100 + '%');

    bar.selectAll('.tick')
        .data(d3.range(1, 4))
        .enter()
        .append('div')
        .attr('class', 'tick')
        .style('left', d => d * 100 / 4 + '%');

    n.on('click', d => {
      this.scatterPlot.clickOnPoint(d.index);
    });
  }

  private updateIsolateButton() {
    let numPoints = this.selectedPoints.length;
    let isolateButton = this.dom.select('.set-filter');
    let clearButton = this.dom.select('button.clear-selection');
    if (numPoints > 1) {
      isolateButton.text(`Isolate ${numPoints} points`).style('display', null);
      clearButton.style('display', null);
    } else {
      isolateButton.style('display', 'none');
      clearButton.style('display', 'none');
    }
  }

  getPcaSampledDim() { return PCA_SAMPLE_DIM.toLocaleString(); }

  getTsneSampleSize() { return SAMPLE_SIZE.toLocaleString(); }

  private getCentroid(pattern: string): CentroidResult {
    let accessor = (a: DataPoint) => a.vector;
    if (pattern == null) {
      return {numMatches: 0};
    }
    if (pattern === '') {
      if (this.allCentroid == null) {
        this.allCentroid =
            vector.centroid(this.points, () => true, accessor).centroid;
      }
      return {centroid: this.allCentroid, numMatches: this.points.length};
    }

    let regExp: RegExp;
    let predicate: (a: DataPoint) => boolean;
    // Check for a regex.
    if (pattern.charAt(0) === '/' &&
        pattern.charAt(pattern.length - 1) === '/') {
      pattern = pattern.slice(1, pattern.length - 1);
      try {
        regExp = new RegExp(pattern, 'i');
      } catch (e) {
        return {error: e.message};
      }
      predicate =
          (a: DataPoint) => { return regExp.test('' + a.metadata['label']); };
      // else does an exact match
    } else {
      predicate = (a: DataPoint) => { return a.metadata['label'] === pattern; };
    }
    return vector.centroid(this.points, predicate, accessor);
  }

  /**
   * Gets the current view of the embedding and saves it as a State object.
   */
  getCurrentState(): State {
    let state: State = {};

    // Save the individual datapoint projections.
    state.projections = [];
    for (let i = 0; i < this.currentDataSet.points.length; i++) {
      state.projections.push(this.currentDataSet.points[i].projections);
    }

    // Save the type of projection.
    state.selectedProjection = this.selectedProjection;

    // Save the selected points.
    state.selectedPoints = this.selectedPoints;

    // Save the camera position and target.
    state.cameraPosition = this.scatterPlot.getCameraPosition();
    state.cameraTarget = this.scatterPlot.getCameraTarget();

    return state;
  }

  /** Loads a State object into the world. */
  loadState(state: State) {
    // Load the individual datapoint projections.
    for (let i = 0; i < state.projections.length; i++) {
      this.currentDataSet.points[i].projections = state.projections[i];
    }

    // Select the type of projection.
    if (state.selectedProjection === 'pca') {
      this.showPCA();
    } else if (state.selectedProjection === 'tsne') {
      this.currentDataSet.hasTSNERun = true;
      this.showTSNE();
    } else if (state.selectedProjection === 'custom') {
      this.showCustom();
    }
    this.showTab(state.selectedProjection);

    // Load the selected points.
    this.selectedPoints = state.selectedPoints;
    this.scatterPlot.clickOnPoint(this.selectedPoints[0]);

    // Load the camera position and target.
    this.scatterPlot.setCameraPositionAndTarget(
        state.cameraPosition, state.cameraTarget);
  }
}

type CentroidResult = {
  centroid?: number[]; numMatches?: number; error?: string
};

document.registerElement(Projector.prototype.is, Projector);
